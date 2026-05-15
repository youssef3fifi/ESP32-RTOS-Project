#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <LiquidCrystal_I2C.h>

// --- 1. تعريف الرجول (Pin Mapping) ---
#define GAS_PIN 34     // حساس الغاز (Analog)
#define VIB_PIN 14     // حساس الاهتزاز (Digital - ISR)
#define BUZZER_PIN 32  // السرينة (Output)
#define LED_R 25       // الليد الأحمر
#define LED_G 26       // الليد الأخضر

// --- 2. تعريف الأجهزة (Hardware Objects) ---
Adafruit_BME280 bme; 
LiquidCrystal_I2C lcd(0x27, 16, 2); // جرب 0x3F لو 0x27 مشتغلتش

// --- 3. تعريف كائنات الـ RTOS (Kernel Objects) ---
QueueHandle_t sensorQueue;
SemaphoreHandle_t vibSemaphore;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t dataMutex; 
SemaphoreHandle_t i2cMutex;

// --- 4. هيكل البيانات المنقولة (Data Structure) ---
struct SensorData {
  int sensorType; // 1: Gas, 2: Vib, 3: Temp
  float value;
  unsigned long sendTime; 
};

// --- 5. المتغيرات العامة والقراءات (Global State) ---
volatile int systemState = 0; // 0: Safe, 1: Warning, 2: Danger
float logGas = 0, logTemp = 0;
int logVib = 0;

// متغيرات قياس الوقت (Timing Metrics)
volatile unsigned long isrTimestamp = 0; 
unsigned long semWakeupDelay = 0;
unsigned long queueLatency = 0;
unsigned long processingWCET = 0;

// ==========================================
// --- Phase 2: Unit Testing (اختبار المنطق) ---
// ==========================================
bool isGasDanger(float gasVal) { return (gasVal > 2500); }
bool isTempDanger(float tempVal) { return (tempVal > 45.0); }

int decideSystemState(float gasLevel, bool vibAlert, bool tempAlert) {
  if (gasLevel > 2500 || vibAlert || tempAlert) return 2; // Danger
  if (gasLevel > 1200) return 1; // Warning
  return 0; // Safe
}

void runUnitTests() {
  Serial.println("===============================");
  Serial.println("   PHASE 2: UNIT TESTS RUNNING ");
  Serial.println("===============================");
  if (isGasDanger(3000) == true) Serial.println("[PASS] Test 1: Gas Danger logic OK");
  if (isTempDanger(30.0) == false) Serial.println("[PASS] Test 2: Temp Safe logic OK");
  if (decideSystemState(500, true, false) == 2) Serial.println("[PASS] Test 3: Decision logic OK");
  Serial.println("===============================\n");
  delay(2000); 
}

// ==========================================
// --- ISR: Interrupt Service Routine ---
// ==========================================
void IRAM_ATTR vibInterrupt() {
  isrTimestamp = micros(); 
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(vibSemaphore, &xHigherPriorityTaskWoken); 
  if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// ==========================================
// --- Tasks Definitions (المهام) ---
// ==========================================

// 1. مهمة حساس الغاز (Analog Task)
void TaskAnalog(void *pvParameters) {
  for (;;) {
    SensorData data;
    data.sensorType = 1;
    data.value = analogRead(GAS_PIN);
    data.sendTime = micros(); 
    xQueueSend(sensorQueue, &data, 0); 
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
}

// 2. مهمة حساس الاهتزاز (Digital Task)
void TaskDigital(void *pvParameters) {
  for (;;) {
    if (xSemaphoreTake(vibSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (isrTimestamp > 0) {
        semWakeupDelay = micros() - isrTimestamp;
        isrTimestamp = 0;
      }
      SensorData data = {2, 1.0, micros()}; 
      xQueueSend(sensorQueue, &data, 0);
    }
  }
}

// 3. مهمة حساس الحرارة (Comm Task)
void TaskComm(void *pvParameters) {
  for (;;) {
    SensorData data;
    data.sensorType = 3;
    data.sendTime = micros();
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      data.value = bme.readTemperature();
      xSemaphoreGive(i2cMutex);
    }
    xQueueSend(sensorQueue, &data, 0);
    vTaskDelay(pdMS_TO_TICKS(500)); 
  }
}

// 4. مهمة المعالجة واتخاذ القرار (Processing Task)
void TaskProcessing(void *pvParameters) {
  float lastGasValue = 0; 
  bool vibAlert = false, tempAlert = false;

  for (;;) {
    SensorData receivedData;
    if (xQueueReceive(sensorQueue, &receivedData, pdMS_TO_TICKS(200)) == pdPASS) {
      unsigned long startTime = micros(); 
      queueLatency = startTime - receivedData.sendTime;

      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        if (receivedData.sensorType == 1) { lastGasValue = receivedData.value; logGas = receivedData.value; }
        if (receivedData.sensorType == 2) { vibAlert = true; logVib = 1; }
        if (receivedData.sensorType == 3) { tempAlert = isTempDanger(receivedData.value); logTemp = receivedData.value; }
        xSemaphoreGive(dataMutex);
      }
      
      systemState = decideSystemState(lastGasValue, vibAlert, tempAlert);

      unsigned long execTime = micros() - startTime;
      if (execTime > processingWCET) processingWCET = execTime;

      if (vibAlert) { vTaskDelay(pdMS_TO_TICKS(2000)); vibAlert = false; } 
    }
  }
}

// 5. مهمة التحكم في المخرجات (Output Task)
void TaskOutput(void *pvParameters) {
  for (;;) {
    if (systemState == 2) { 
      digitalWrite(LED_R, HIGH); digitalWrite(LED_G, LOW);
      tone(BUZZER_PIN, 1000);
    } else if (systemState == 1) { 
      digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); 
      digitalWrite(BUZZER_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(BUZZER_PIN, LOW);
    } else { 
      digitalWrite(LED_R, LOW); digitalWrite(LED_G, HIGH);
      noTone(BUZZER_PIN); digitalWrite(BUZZER_PIN, LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// 6. مهمة الطباعة والتشخيص الشاملة (Logging Task)
void TaskLogging(void *pvParameters) {
  for (;;) {
    float pGas, pTemp; int pVib;
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      pGas = logGas; pTemp = logTemp; pVib = logVib;
      if (pVib == 1) logVib = 0; 
      xSemaphoreGive(dataMutex);
    }

    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
      Serial.print("[LOG] Gas: "); Serial.print(pGas);
      Serial.print(" | Temp: "); Serial.print(pTemp); Serial.print("C");
      Serial.print(" | Vib: "); Serial.print(pVib ? "DETECTED" : "OK");
      Serial.print(" | State: "); 
      if(systemState == 2) Serial.print("DANGER");
      else if(systemState == 1) Serial.print("WARNING");
      else Serial.print("SAFE");
      Serial.print(" | Q_Free: "); Serial.print(uxQueueSpacesAvailable(sensorQueue));
      Serial.print(" || WCET: "); Serial.print(processingWCET); Serial.print("us");
      Serial.print(" | Q_Lat: "); Serial.print(queueLatency); Serial.print("us");
      Serial.print(" | Sem_Wake: "); Serial.print(semWakeupDelay); Serial.print("us");
      Serial.println();
      xSemaphoreGive(serialMutex);
    }

    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("G:"); lcd.print((int)pGas);
      lcd.print(" T:"); lcd.print(pTemp, 1);
      lcd.setCursor(0, 1); lcd.print("St:"); 
      if(systemState == 2) lcd.print("DANGER ");
      else if(systemState == 1) lcd.print("WARN   ");
      else lcd.print("SAFE   ");
      if(pVib) lcd.print("!VIB!");
      xSemaphoreGive(i2cMutex); 
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); 
  }
}

// ==========================================
// --- Setup & Main ---
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT); pinMode(VIB_PIN, INPUT_PULLUP);
  
  lcd.init(); lcd.backlight();
  if (!bme.begin(0x76)) Serial.println("BME280 error");

  runUnitTests(); 
  attachInterrupt(digitalPinToInterrupt(VIB_PIN), vibInterrupt, RISING);

  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  vibSemaphore = xSemaphoreCreateBinary();
  serialMutex = xSemaphoreCreateMutex();
  dataMutex = xSemaphoreCreateMutex(); 
  i2cMutex = xSemaphoreCreateMutex();

  if(sensorQueue != NULL && vibSemaphore != NULL && i2cMutex != NULL) {
    xTaskCreate(TaskAnalog, "Analog", 2048, NULL, 2, NULL);
    xTaskCreate(TaskDigital, "Digital", 2048, NULL, 4, NULL);
    xTaskCreate(TaskComm, "Comm", 2048, NULL, 2, NULL);
    xTaskCreate(TaskProcessing, "Processing", 2048, NULL, 3, NULL);
    xTaskCreate(TaskOutput, "Output", 2048, NULL, 3, NULL);
    xTaskCreate(TaskLogging, "Logging", 2048, NULL, 1, NULL);
  }
}

void loop() {}