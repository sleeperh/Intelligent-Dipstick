#include <arduinoFFT.h>
#include <NimBLEDevice.h>
#include <Thermocouple.h>
#include <MAX6675_Thermocouple.h>

static NimBLEServer* pServer;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Thermocouple Sensor Pins:
#define SCK_PIN 12
#define CS_PIN 5
#define SO_PIN 4

// FFT 
#define SAMPLES 1024 // Must be a power of 2
#define SAMPLING_FREQ 40000 // Hz, must be 40000 or less due to ADC conversion time. Determines maximum frequency that can be analysed by the FFT Fmax=sampleF/2.
#define AUDIO_IN_PIN 25 // Signal in on this pin
#define NUM_BANDS 16 // To change this, you will need to change the bunch of if statements describing the mapping from bins to bands
#define NOISE 5000 // Used as a crude noise filter, values below this are ignored

// LED 
const int redLEDPin =  27;
const int greenLEDPin = 26;

// Sampling and FFT stuff
unsigned int sampling_period_us;
byte peak[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; // The length of these arrays must be >= NUM_BANDS
int bandValues[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}; 
int freqBins[] = {336,339,342,346,349,352,355,359,362,366,369,373,376,380,383,387};
double vReal[SAMPLES];
double vImag[SAMPLES];
unsigned long newTime;

// Reading Interval Settings
int readingsInSecs = 20; // interval in seconds at which temperature data is sent to Gateway
int interval = readingsInSecs * 1000;
int soundInterval = 10; // interval in ms at which the sound is checked via FFT
unsigned long previousMillis = 0;

// Sensors
Thermocouple* thermocouple;
String temp;
arduinoFFT FFT = arduinoFFT(vReal, vImag, SAMPLES, SAMPLING_FREQ);
String pingNoise = "0";

class ServerCallbacks: public NimBLEServerCallbacks 
{
    void onConnect(NimBLEServer* pServer) 
    {
        Serial.println("Client connected");
        Serial.println("Multi-connect support: start advertising");
        deviceConnected = true;
        NimBLEDevice::startAdvertising();
    };

    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) 
    {
        Serial.print("Client address: ");
        Serial.println(NimBLEAddress(desc->peer_ota_addr).toString().c_str());
        deviceConnected = true;
        pServer->updateConnParams(desc->conn_handle, 24, 48, 0, 60);
    };
    void onDisconnect(NimBLEServer* pServer) 
    {
        Serial.println("Client disconnected - start advertising");
        NimBLEDevice::startAdvertising();
        deviceConnected = false;
    };
    void onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) 
    {
        Serial.printf("MTU updated: %u for connection ID: %u\n", MTU, desc->conn_handle);
    };
};

// Handler class for characteristic actions 
class CharacteristicCallbacks: public NimBLECharacteristicCallbacks 
{
    void onRead(NimBLECharacteristic* pCharacteristic)
    {
        Serial.print(pCharacteristic->getUUID().toString().c_str());
        Serial.print(": onRead(), value: ");
        Serial.println(pCharacteristic->getValue().c_str());
    };

    void onWrite(NimBLECharacteristic* pCharacteristic) 
    {
        Serial.print(pCharacteristic->getUUID().toString().c_str());
        Serial.print(": onWrite(), value: ");
        Serial.println(pCharacteristic->getValue().c_str());
    };
    // Called before notification or indication is sent, 
    // the value can be changed here before sending if desired.
    
    void onNotify(NimBLECharacteristic* pCharacteristic) 
    {
        Serial.println("Sending notification to clients");
    };

    // The status returned in status is defined in NimBLECharacteristic.h.
    // The value returned in code is the NimBLE host return code.
    void onStatus(NimBLECharacteristic* pCharacteristic, Status status, int code)
    {
        String str = ("Notification/Indication status code: ");
        str += status;
        str += ", return code: ";
        str += code;
        str += ", "; 
        str += NimBLEUtils::returnCodeToString(code);
        Serial.println(str);
    };

    void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) 
    {
        String str = "Client ID: ";
        str += desc->conn_handle;
        str += " Address: ";
        str += std::string(NimBLEAddress(desc->peer_ota_addr)).c_str();
        if(subValue == 0) 
        {
            str += " Unsubscribed to ";
        }
        else if(subValue == 1) 
        {
            str += " Subscribed to notfications for ";
        } 
        else if(subValue == 2) 
        {
            str += " Subscribed to indications for ";
        } 
        else if(subValue == 3) 
        {
            str += " Subscribed to notifications and indications for ";
        }
        str += std::string(pCharacteristic->getUUID()).c_str();

        Serial.println(str);
    };
};
    
// Handler class for descriptor actions  
class DescriptorCallbacks : public NimBLEDescriptorCallbacks 
{
    void onWrite(NimBLEDescriptor* pDescriptor) 
    {
        std::string dscVal((char*)pDescriptor->getValue(), pDescriptor->getLength());
        Serial.print("Descriptor witten value:");
        Serial.println(dscVal.c_str());
    };

    void onRead(NimBLEDescriptor* pDescriptor) 
    {
        Serial.print(pDescriptor->getUUID().toString().c_str());
        Serial.println(" Descriptor read");
    };
};

// Define callback instances globally to use for multiple Charateristics \ Descriptors 
static DescriptorCallbacks dscCallbacks;
static CharacteristicCallbacks chrCallbacks;


void setup() 
{
    Serial.begin(115200);
    pinMode(redLEDPin, OUTPUT); 
    pinMode(greenLEDPin, OUTPUT); 
    digitalWrite(greenLEDPin, LOW); 
    digitalWrite(redLEDPin, HIGH);
    Serial.println("Starting NimBLE Server");
    sampling_period_us = round(1000000 * (1.0 / SAMPLING_FREQ));
    // sets device name 
    NimBLEDevice::init("IDS-Sensor");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);//  +9db 
    NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_SC);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = pServer->createService("fde9de3c-8787-11ec-a8a3-0242ac120002");
    NimBLECharacteristic* pIDSCharacteristic = pService->createCharacteristic("c91e0eb2-8782-11ec-a8a3-0242ac120002",NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);

    pService->start(); // Start the services when finished creating all Characteristics and Descriptors 
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(pService->getUUID()); // Add the services to the advertisment data 
    pAdvertising->setScanResponse(true);
    pAdvertising->start();
    Serial.println("Advertising Started");
}

void loop() 
{
  // Send notifications to connected clients 
    if(pServer->getConnectedCount()) // if connected
    {
        NimBLEService* pSvc = pServer->getServiceByUUID("fde9de3c-8787-11ec-a8a3-0242ac120002"); // get service
        if(pSvc) // connected to the service?
        {
            NimBLECharacteristic* pChr = pSvc->getCharacteristic("c91e0eb2-8782-11ec-a8a3-0242ac120002"); // get characteristic
            if(pChr) // connected to the characteristic?
            {
                unsigned long currentMillis = millis();
                if(currentMillis - previousMillis >= soundInterval) 
                {
                    pingNoise = SoundFFT();
                    String noiseAlert = "Sound: ";
                    if (pingNoise == "1")
                    {
                        // Indicate data being sent
                        digitalWrite(greenLEDPin, LOW);
                        digitalWrite(redLEDPin, HIGH);
                        noiseAlert += pingNoise;
                        char bufS[20]; // Declare char array
                        noiseAlert.toCharArray(bufS, 14);
                        pChr->setValue(bufS);
                        pChr->notify(true);
                        digitalWrite(greenLEDPin, HIGH);
                        digitalWrite(redLEDPin, LOW);
                    }
                }

                if(currentMillis - previousMillis >= interval) 
                {
                    // Indicate data being sent
                    digitalWrite(greenLEDPin, LOW);
                    digitalWrite(redLEDPin, HIGH);
                    
                    String noiseAlert = "Sound: ";
                    noiseAlert += pingNoise;
                    char bufS[20];
                    noiseAlert.toCharArray(bufS, 14);
                    pChr->setValue(bufS);
                    pChr->notify(true);
                    char bufT[20]; // Declare char array
                    temp = GetTempReadings(); // get temperature readings in String form
                    Serial.println(temp);
                    if(temp == "Temp 1: nan")
                    {
                        String ErrorAlrt = "Error: 1";
                        Serial.println(ErrorAlrt);
                        char bufE[20];
                        ErrorAlrt.toCharArray(bufE, 14);
                        pChr->setValue(bufE);
                        pChr->notify(true);
                    }
                    else
                    {
                        String ErrorAlrt = "Error: 0";
                        char bufE[20];
                        ErrorAlrt.toCharArray(bufE, 14);
                        pChr->setValue(bufE);
                        pChr->notify(true);
                    }
                    temp.toCharArray(bufT, 14); // convert temperature to char array
                    pChr->setValue(bufT); // set value to temperature reading for notification
                    pChr->notify(true); // notify client of new temperature data
                    // Indicate completed action
                    delay(500);
                    digitalWrite(greenLEDPin, HIGH);
                    digitalWrite(redLEDPin, LOW);
                    previousMillis = currentMillis; // reset timer
                }
            }
        }
    }
    if (deviceConnected)
    {
        digitalWrite(greenLEDPin, HIGH); 
        digitalWrite(redLEDPin, LOW);
    }
    if (!deviceConnected && oldDeviceConnected)
    {
        oldDeviceConnected = deviceConnected;
        digitalWrite(greenLEDPin, LOW); 
        digitalWrite(redLEDPin, HIGH);
    }
    if (deviceConnected && !oldDeviceConnected)
    {
        oldDeviceConnected = deviceConnected;
    }
}

String GetTempReadings()
{
    // Getting temperature data
    thermocouple = new MAX6675_Thermocouple(SCK_PIN, CS_PIN, SO_PIN);
    const double fahrenheit = thermocouple->readFahrenheit();
    // Convert to string with identifier and print to serial
    String TempRead = "Temp 1: " + String(fahrenheit);
    return TempRead;
}

String SoundFFT()
{
  // reset bandValues array
    for (int i = 0; i<NUM_BANDS; i++)
    {
        bandValues[i] = 0;
    }
  
    //Sample the audio 
    for (int i = 0; i < SAMPLES; i++) 
    {
        newTime = micros();
        vReal[i] = analogRead(AUDIO_IN_PIN); // A conversion takes about 9.7uS on an ESP32
        vImag[i] = 0;
        while ((micros() - newTime) < sampling_period_us) { /* chill */ }
    }
    
    // Compute FFT
    FFT.DCRemoval(); // removes DC component from sample data
    FFT.Windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD); // performs windowing function on the values array removing sharp transitions between samples
    FFT.Compute(FFT_FORWARD); // compute FFT
    FFT.ComplexToMagnitude(); // compute magnetudes
  
    // Analyse FFT results
    for (int i = 2; i < (SAMPLES/2); i++) // Don't use sample 0 and only first SAMPLES/2 are usable. Each array element represents a frequency bin and its value the amplitude.
    {                           
        if (vReal[i] > NOISE) // Add a crude noise filter
        {                    
            if (i <= freqBins[0]) bandValues[0] += (int)vReal[i];
            if (i > freqBins[0] && i <= freqBins[1]) bandValues[1] += (int)vReal[i];
            if (i > freqBins[1] && i <= freqBins[2]) bandValues[2] += (int)vReal[i];
            if (i > freqBins[2] && i <= freqBins[3]) bandValues[3] += (int)vReal[i];
            if (i > freqBins[3] && i <= freqBins[4]) bandValues[4] += (int)vReal[i];
            if (i > freqBins[4] && i <= freqBins[5]) bandValues[5] += (int)vReal[i];
            if (i > freqBins[5] && i <= freqBins[6]) bandValues[6] += (int)vReal[i];
            if (i > freqBins[6] && i <= freqBins[7]) bandValues[7] += (int)vReal[i];
            if (i > freqBins[7] && i <= freqBins[8]) bandValues[8] += (int)vReal[i];
            if (i > freqBins[8] && i <= freqBins[9]) bandValues[9] += (int)vReal[i];
            if (i > freqBins[9] && i <= freqBins[10]) bandValues[10] += (int)vReal[i];
            if (i > freqBins[10] && i <= freqBins[11]) bandValues[11] += (int)vReal[i];
            if (i > freqBins[11] && i <= freqBins[12]) bandValues[12] += (int)vReal[i];
            if (i > freqBins[12] && i <= freqBins[13]) bandValues[13] += (int)vReal[i];
            if (i > freqBins[13] && i <= freqBins[14]) bandValues[14] += (int)vReal[i];
            if (i > freqBins[14]) bandValues[15] += (int)vReal[i];
        }
    }
    if (bandValues[7] >= 5000 && bandValues[6] >= 5000)
    {
        Serial.println("Ping!");
        return "1";
    }
    else
        return "0";
}
