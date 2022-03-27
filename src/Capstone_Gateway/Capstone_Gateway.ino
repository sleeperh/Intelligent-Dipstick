#include <NimBLEDevice.h>
#include <Notecard.h>

#define serialDebug Serial

Notecard notecard;

#define productUID "com.your-company.your-name:you_project"

void scanEndedCB(NimBLEScanResults results);

static NimBLEAdvertisedDevice* advDevice;

bool enableBlues = true;
static bool doConnect = false;
static uint32_t scanTime = 0; /** 0 = scan forever */

String charsToString;
double temp1;
double sound;

const int REDLEDPIN = 13;
const int GREENLEDPIN = 12;

int sensorCount = 0;
int sensorMax = 1;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ClientCallbacks : public NimBLEClientCallbacks 
{
    void onConnect(NimBLEClient* pClient) 
    {
        serialDebug.println("Connected");
        /** After connection we should change the parameters if we don't need fast response times.
         *  These settings are 150ms interval, 0 latency, 450ms timout.
         *  Timeout should be a multiple of the interval, minimum is 100ms.
         *  I find a multiple of 3-5 * the interval works best for quick response/reconnect.
         *  Min interval: 120 * 1.25ms = 150, Max interval: 120 * 1.25ms = 150, 0 latency, 60 * 10ms = 600ms timeout
         */
        pClient->updateConnParams(120,120,0,60);
    };

    void onDisconnect(NimBLEClient* pClient) 
    {
        digitalWrite(REDLEDPIN, HIGH);
        sensorCount = sensorCount - 1;
        serialDebug.println("Sensor count: " + String(sensorCount));
        serialDebug.print(pClient->getPeerAddress().toString().c_str());
        serialDebug.println(" Disconnected - Starting scan");
        if(sensorCount == 0)
        {
          digitalWrite(GREENLEDPIN, LOW);
        }
        NimBLEDevice::getScan()->start(scanTime, scanEndedCB);
    };

    /** Called when the peripheral requests a change to the connection parameters.
     *  Return true to accept and apply them or false to reject and keep
     *  the currently used parameters. Default will return true.
     */
    bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) 
    {
        if(params->itvl_min < 24) { /** 1.25ms units */
            return false;
        } else if(params->itvl_max > 40) { /** 1.25ms units */
            return false;
        } else if(params->latency > 2) { /** Number of intervals allowed to skip */
            return false;
        } else if(params->supervision_timeout > 100) { /** 10ms units */
            return false;
        }

        return true;
    };

    /********************* Security handled here **********************
    ****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest()
    {
        serialDebug.println("Client Passkey Request");
        /** return the passkey to send to the server */
        return 123456;
    };

    bool onConfirmPIN(uint32_t pass_key)
    {
        serialDebug.print("The passkey YES/NO number: ");
        serialDebug.println(pass_key);
    /** Return false if passkeys don't match. */
        return true;
    };

    /** Pairing process complete, we can check the results in ble_gap_conn_desc */
    void onAuthenticationComplete(ble_gap_conn_desc* desc)
    {
        if(!desc->sec_state.encrypted) 
        {
            serialDebug.println("Encrypt connection failed - disconnecting");
            /** Find the client with the connection handle provided in desc */
            NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
            return;
        }
    };
};


/** Define a class to handle the callbacks when advertisments are received */
class AdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks 
{
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) 
    {
        serialDebug.print("Advertised Device found: ");
        serialDebug.println(advertisedDevice->toString().c_str());
        if(advertisedDevice->isAdvertisingService(NimBLEUUID("fde9de3c-8787-11ec-a8a3-0242ac120002")))
        {
            serialDebug.println("Found Our Service");
            /** stop scan before connecting */
            NimBLEDevice::getScan()->stop();
            /** Save the device reference in a global for the client to use*/
            advDevice = advertisedDevice;
            /** Ready to connect now */
            doConnect = true;
        }
    };
};


/** Notification / Indication receiving handler callback */
void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify)
{
    const char* payload = (char*)pData; // convert pData to constant char
    charsToString = ((const __FlashStringHelper*) payload); // convert payload const char to String

    // Determine from which sensor data is being recieved
    if(charsToString.startsWith("Temp 1:"))
    {
      temp1 = charsToString.substring(8).toDouble();
    }
    if(charsToString.startsWith("Sound:"))
    {
      sound = charsToString.substring(7).toDouble();
    }

    serialDebug.println("Temp 1: " + String(temp1));
    serialDebug.println("Sound: " + String(sound) + "\n");
    if(enableBlues)
    {
        J *req = notecard.newRequest("note.add");
        if (req != NULL) 
        {
            JAddStringToObject(req, "file", "sensors.qo");
            JAddBoolToObject(req, "sync", true);
      
            J *body = JCreateObject();
            if (body != NULL) 
            {
                JAddNumberToObject(body, "temp1", temp1);
                JAddNumberToObject(body, "sound", sound);
                JAddItemToObject(req, "body", body);
            }
            notecard.sendRequest(req);
            digitalWrite(GREENLEDPIN, LOW);
            digitalWrite(REDLEDPIN, HIGH);
            delay(1000);
            digitalWrite(GREENLEDPIN, HIGH);
            digitalWrite(REDLEDPIN, LOW);
        }
    }
}

/** Callback to process the results of the last scan or restart it */
void scanEndedCB(NimBLEScanResults results)
{
  serialDebug.println("Scan Ended");
}


/** Create a single global instance of the callback class to be used by all clients */
static ClientCallbacks clientCB;


/** Handles the provisioning of clients and connects / interfaces with the server */
bool connectToServer() 
{
    NimBLEClient* pClient = nullptr;

    /** Check if we have a client we should reuse first **/
    if(NimBLEDevice::getClientListSize()) 
    {
        /** Special case when we already know this device, we send false as the
         *  second argument in connect() to prevent refreshing the service database.
         *  This saves considerable time and power.
         */
        pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if(pClient)
        {
            if(!pClient->connect(advDevice, false)) 
            {
                serialDebug.println("Reconnect failed");
                return false;
            }
            serialDebug.println("Reconnected client");
        }
        /** We don't already have a client that knows this device,
         *  we will check for a client that is disconnected that we can use.
         */
        else 
        {
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }

    /** No client to reuse? Create a new one. */
    if(!pClient) 
    {
        if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) 
        {
            serialDebug.println("Max clients reached - no more connections available");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        serialDebug.println("New client created");

        pClient->setClientCallbacks(&clientCB, false);
        /** Set initial connection parameters: These settings are 15ms interval, 0 latency, 120ms timout.
         *  These settings are safe for 3 clients to connect reliably, can go faster if you have less
         *  connections. Timeout should be a multiple of the interval, minimum is 100ms.
         *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
         */
        pClient->setConnectionParams(12,12,0,51);
        /** Set how long we are willing to wait for the connection to complete (seconds), default is 30. */
        pClient->setConnectTimeout(5);


        if (!pClient->connect(advDevice)) 
        {
            /** Created a client but failed to connect, don't need to keep it as it has no data */
            NimBLEDevice::deleteClient(pClient);
            serialDebug.println("Failed to connect, deleted client");
            return false;
        }
    }

    if(!pClient->isConnected()) 
    {
        if (!pClient->connect(advDevice)) 
        {
            serialDebug.println("Failed to connect");
            return false;
        }
    }

    serialDebug.print("Connected to: ");
    serialDebug.println(pClient->getPeerAddress().toString().c_str());
    serialDebug.print("RSSI: ");
    serialDebug.println(pClient->getRssi());

    /** Now we can read/write/subscribe the charateristics of the services we are interested in */
    NimBLERemoteService* pSvc = nullptr;
    NimBLERemoteCharacteristic* pChr = nullptr;
    NimBLERemoteDescriptor* pDsc = nullptr;


    pSvc = pClient->getService("fde9de3c-8787-11ec-a8a3-0242ac120002");
    if(pSvc) 
    {     /** make sure it's not null */
        pChr = pSvc->getCharacteristic("c91e0eb2-8782-11ec-a8a3-0242ac120002");

        if(pChr) 
        {     /** make sure it's not null */
            if(pChr->canRead()) 
            {
                serialDebug.print(pChr->getUUID().toString().c_str());
                serialDebug.print(" Value: ");
                serialDebug.println(pChr->readValue().c_str());
            }

            pDsc = pChr->getDescriptor(NimBLEUUID("C01D"));
            if(pDsc) 
            {   /** make sure it's not null */
                serialDebug.print("Descriptor: ");
                serialDebug.print(pDsc->getUUID().toString().c_str());
                serialDebug.print(" Value: ");
                serialDebug.println(pDsc->readValue().c_str());
            }

            /** registerForNotify() has been deprecated and replaced with subscribe() / unsubscribe().
             *  Subscribe parameter defaults are: notifications=true, notifyCallback=nullptr, response=false.
             *  Unsubscribe parameter defaults are: response=false.
             */
            if(pChr->canNotify()) 
            {
                if(!pChr->subscribe(true, notifyCB)) 
                {
                    /** Disconnect if subscribe failed */
                    pClient->disconnect();
                    return false;
                }
            }
            else if(pChr->canIndicate()) 
            {
                /** Send false as first argument to subscribe to indications instead of notifications */
                if(!pChr->subscribe(false, notifyCB)) 
                {
                    /** Disconnect if subscribe failed */
                    pClient->disconnect();
                    return false;
                }
            }
        }
    } 
    else 
    {
        serialDebug.println("Service not found.");
    }

    serialDebug.println("Done with this device!");
    return true;
}

void setup ()
{
    pinMode(REDLEDPIN, OUTPUT); 
    pinMode(GREENLEDPIN, OUTPUT); 
  
    #ifdef serialDebug
      digitalWrite(GREENLEDPIN, HIGH);
      digitalWrite(REDLEDPIN, LOW);
      delay(500);
      digitalWrite(GREENLEDPIN, LOW); 
      digitalWrite(REDLEDPIN, HIGH);
      delay(500);
      digitalWrite(GREENLEDPIN, HIGH);
      digitalWrite(REDLEDPIN, LOW);
      delay(500);
      digitalWrite(GREENLEDPIN, LOW); 
      digitalWrite(REDLEDPIN, HIGH);
      delay(500);
      digitalWrite(GREENLEDPIN, HIGH);
      digitalWrite(REDLEDPIN, LOW);
      delay(500);
      digitalWrite(GREENLEDPIN, LOW); 
      digitalWrite(REDLEDPIN, HIGH);
      serialDebug.begin(115200);
      notecard.setDebugOutputStream(serialDebug);
    #endif
    
    // Initialize the physical I/O channel to the Notecard
    #ifdef serialNotecard
      notecard.begin(serialNotecard, 9600);
    #else
      notecard.begin();
    #endif

    J *req = notecard.newRequest("hub.set");
    JAddStringToObject(req, "product", productUID);
    JAddStringToObject(req, "mode", "continuous");
    JAddStringToObject(req, "sync", "true");
    //JAddNumberToObject(req, "outbound", 1);
    notecard.sendRequest(req);
    
    serialDebug.println("Starting NimBLE Client");
    /** Initialize NimBLE, no device name spcified as we are not advertising */
    NimBLEDevice::init("");

    /** Set the IO capabilities of the device, each option will trigger a different pairing method.
     *  BLE_HS_IO_KEYBOARD_ONLY    - Passkey pairing
     *  BLE_HS_IO_DISPLAY_YESNO   - Numeric comparison pairing
     *  BLE_HS_IO_NO_INPUT_OUTPUT - DEFAULT setting - just works pairing
     */
    //NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY); // use passkey
    //NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); //use numeric comparison

    /** 2 different ways to set security - both calls achieve the same result.
     *  no bonding, no man in the middle protection, secure connections.
     *
     *  These are the default values, only shown here for demonstration.
     */
    //NimBLEDevice::setSecurityAuth(false, false, true);
    NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

    /** Optional: set the transmit power, default is 3db */
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */

    /** Optional: set any devices you don't want to get advertisments from */
    // NimBLEDevice::addIgnored(NimBLEAddress ("aa:bb:cc:dd:ee:ff"));

    /** create new scan */
    NimBLEScan* pScan = NimBLEDevice::getScan();

    /** create a callback that gets called when advertisers are found */
    pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());

    /** Set scan interval (how often) and window (how long) in milliseconds */
    pScan->setInterval(45);
    pScan->setWindow(15);

    /** Active scan will gather scan response data from advertisers
     *  but will use more energy from both devices
     */
    pScan->setActiveScan(true);
    /** Start scanning for advertisers for the scan time specified (in seconds) 0 = forever
     *  Optional callback for when scanning stops.
     */
    pScan->start(scanTime, scanEndedCB);
}


void loop ()
{
    /** Loop here until we find a device we want to connect to */
    while(!doConnect)
    {
        delay(1);
    }

    doConnect = false;

    /** Found a device we want to connect to, do it now */
    if(connectToServer()) 
    {
        serialDebug.println("Success! we should now be getting notifications, scanning for more!");
        sensorCount++;
        serialDebug.println("Sensor Count: " + String(sensorCount));
    } 
    else 
    {
        serialDebug.println("Failed to connect, starting scan");
    }

    if(sensorCount < sensorMax && sensorCount != 0) // new code: stops scanning once max servers/sensors are connected
    {
        digitalWrite(GREENLEDPIN, HIGH);
        digitalWrite(REDLEDPIN, HIGH);
        NimBLEDevice::getScan()->start(scanTime,scanEndedCB); // this code used to stand alone outside of any if statement
    }
    if(sensorCount == sensorMax)
    {
        digitalWrite(GREENLEDPIN, HIGH);
        digitalWrite(REDLEDPIN, LOW);
        serialDebug.println("Client limit reached! No more scanning");
    }

}
