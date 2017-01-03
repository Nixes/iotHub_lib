#include "ArduinoJson.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <aWOT.h>

// both these values are currently unused
#define wifi_connection_time 2000 // how long it takes on average to reconnect to wifi
#define sensor_aquisition_time 2000 // how long it takes to retrieve the sensor values
#define max_node_name_length 100 // the maxiumum length of a nodes (sensor / actor) name


struct sensor {
  char id[25];
  char name[100]; // sensor name limited to 99 characters
};
struct actor {
  char id[25];
  const char* name; // actor name limited to 99 characters
  // for good example of using these "tagged unions" go to: http://stackoverflow.com/questions/18577404/how-can-a-mixed-data-type-int-float-char-etc-be-stored-in-an-array
  enum {is_int, is_float, is_bool} state_type;
  union {
    int istate;
    double fstate;
    bool bstate;
  } state;
  // pointer to a function that is run when a new actor state is received, this will also vary by state_type
  union {
    void (*icallback)(int);
    void (*fcallback)(double);
    void (*bcallback)(bool);
  } on_update;
};

template<const uint number_sensor_ids,const uint number_actor_ids> class iotHubLib {
private:
  char* iothub_server; // the location of the server
  int iothub_port;

  WiFiServer server{80}; // the server that accepts requests from the hub, note the () initialisation syntax in not supported in class bodies
  WebApp app; // the class used by aWOT

  uint sleep_interval = 30000; // default of 30 seconds
  const int ids_eeprom_offset = 1; // memory location for ids start +1, skipping zero
  char sensor_ids[number_sensor_ids][25]; // array of sensor ID's, sensor ids are 24 alphanumeric keys long, the extra char is for the null character
  //char actor_ids[number_actor_ids][25];
  char sensor_names[number_sensor_ids][25]; // array of sensor names
  uint last_sensor_added_index;


  // going to replace the id arrays with custom structs instead
  //sensor sensors[number_sensor_ids];
  actor actors[number_actor_ids];
  uint last_actor_added_index; // the index of the last actor added

  static void GetActorsHandler(Request &req, Response &res) {
    Serial.printf("Root Page Requested");
    // P macro for printing strings from program memory
   P(index) =
     "<html>\n"
     "<head>\n"
     "<title>IOTHub Node</title>\n"
     "</head>\n"
     "<body>\n"
     "<h1>Test post please ignore!</h1>\n"
     "</body>\n"
     "</html>";

   res.success("text/html");
   res.printP(index);
  }

  void RegisterRouteHandlers() {
    app.get("/", &GetActorsHandler);
    Serial.printf("Routes Registered");
  }

  void CheckConnections() {
    //Serial.print("*");
    WiFiClient client = server.available();
    if (client.available()){
      Serial.printf("Client was available");
      app.process(&client);
    }
    delay(20);
  }

  bool CheckFirstBoot() {
    Serial.print("BootByte: "); Serial.println(EEPROM.read(0));
    // check first byte is set to 128, this indicates this is not the first boot
    if ( 128 == EEPROM.read(0) ) {
      Serial.println("Previous boot detected, loading existing sensor configs");
      return false;
    } else {
      Serial.println("No Previous boot detected");
      return true;
    }
  }

  void UpdateFirstBoot() {
    // check first byte is set to 128, this indicates this is not the first boot
    EEPROM.write(0,128);
    EEPROM.commit();
  }

  void SetFirstBoot() {
    EEPROM.write(0,0);
    EEPROM.commit();
  }

  void ShowEeprom() {
    // first byte reserved
    Serial.println();
    Serial.print("Read bytes: ");

    for(int addr = 0; addr < 48 + ids_eeprom_offset;) {
      Serial.print("[");
      Serial.print( (char)EEPROM.read(addr));
      Serial.print("] ");
      addr++;
    }
    Serial.println("//end");
  }

  // this should read sensor ID's from internal memory if available, else ask for new ids from the given server
  void LoadIds() {
    // first byte reserved
    int addr = ids_eeprom_offset;

    // read sensor ids
    for(int i = 0; i< number_sensor_ids;i++) {
      // read an entire 24 byte id
      for(int j = 0; j < 24;j++) {
          sensor_ids[i][j] = (char)EEPROM.read(addr);
          addr++;
      }
      Serial.print("Read from eeprom into sensor_ids: "); Serial.println(sensor_ids[i]);
    }

    // read actor ids
    for(int i = 0; i< number_actor_ids;i++) {
      // read an entire 24 byte id
      for(int j = 0; j < 24;j++) {
          actors[i].id[j] = (char)EEPROM.read(addr);
          addr++;
      }
      Serial.print("Read from eeprom into actor_ids: "); Serial.println(actors[i].id);
    }

    EEPROM.commit();
    Serial.print("Read bytes: "); Serial.println(addr-ids_eeprom_offset);
  };

  // this should save sensor ID's to internal memory
  void SaveIds() {
    // first byte reserved
    int addr = ids_eeprom_offset;

    // save sensor ids into eeprom
    for(int i = 0; i< number_sensor_ids;i++) {

      for(int j = 0; j < 24;j++) {
          EEPROM.write(addr, sensor_ids[i][j] );
          addr++;
      }

    }

    // save actor ids into eeprom
    for(int i = 0; i< number_actor_ids;i++) {

      for(int j = 0; j < 24;j++) {
          EEPROM.write(addr, actors[i].id[j] );
          addr++;
      }

    }

    EEPROM.commit();
    Serial.print("Wrote bytes: "); Serial.println(addr-ids_eeprom_offset);
  };

  void GetIdFromJson(String json_string, char (*sensor_id)[25]) {
    StaticJsonBuffer<100> jsonBuffer;
    JsonObject& json_object = jsonBuffer.parseObject(json_string);
    const char* id = json_object["id"];
    //strcpy (to,from)
    strcpy (*sensor_id,id);
  }

  void RegisterSensor(const char* sensor_name,char (*sensor_id)[25]) {
    if (strlen(sensor_name) > max_node_name_length) {
      Serial.println("Sensor being registered had a name length over that set by max_node_name_length");
      return;
    }
    // check that we don't already have too many sensors
    if (last_sensor_added_index > number_sensor_ids) {
      Serial.println("Sensor being registered was more than the number specified in initialisation.");
      return;
    }

    Serial.println("Registering sensor");
    HTTPClient http;

    // Make a HTTP post
    http.begin(iothub_server,iothub_port,"/api/sensors");

    // prep the json object
    StaticJsonBuffer<max_node_name_length+10> jsonBuffer;
    JsonObject& json_obj = jsonBuffer.createObject();
    json_obj["name"] = sensor_name;

    http.addHeader("Content-Type","application/json"); // important! JSON conversion in nodejs requires this

    // add the json to a string
    String json_string;
    json_obj.printTo(json_string);// this is great except it seems to be adding quotation marks around what it is sending
    // then send the json
    http.POST(json_string);

    // then print the response over Serial
    Serial.print("Response ID: ");
    GetIdFromJson(http.getString(),*&sensor_id);

    Serial.print(*sensor_id); Serial.println("///end");
    //Serial.println( sensor_id );

    http.end();
  }

  // test that saved sensors Ids are still registered
  void CheckRegistered() {
    HTTPClient http;
    http.begin(iothub_server,iothub_port,"/api/sensors/");
    http.end();
  }

  void BaseRegisterActor(actor *actor_ptr) {
    Serial.println("Registering actor");
    HTTPClient http;

    // Make a HTTP post
    http.begin(iothub_server,iothub_port,"/api/actors");

    // prep the json object
    StaticJsonBuffer<max_node_name_length+10> jsonBuffer;
    JsonObject& json_obj = jsonBuffer.createObject();
    json_obj["name"] = actor_ptr->name;

    http.addHeader("Content-Type","application/json"); // important! JSON conversion in nodejs requires this

    // add the json to a string
    String json_string;
    json_obj.printTo(json_string);// this is great except it seems to be adding quotation marks around what it is sending
    // then send the json
    http.POST(json_string);

    // then print the response over Serial
    Serial.print("Response ID: ");
    GetIdFromJson(http.getString(),&actor_ptr->id);

    Serial.print(*actor_ptr->id); Serial.println("///end");
    //Serial.println( sensor_id );

    http.end();
  }

public:
  // constructor
  iotHubLib(char* tmp_server, int tmp_port) {
    iothub_server = tmp_server;
    iothub_port = tmp_port;
    last_actor_added_index = 0;
    last_sensor_added_index = 0;
  };
  // destructor
  ~iotHubLib() {
  };

  void Start() {
    Serial.begin(115200);
    WiFi.begin(); // wifi configuration is outside the scope of this lib, use whatever was last used

    Serial.println("Establishing Wifi Connection");
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.print(".");
    }
    Serial.println();
    Serial.print("DONE - Got IP: "); Serial.println(WiFi.localIP());

    EEPROM.begin(512); // so we can read / write EEPROM

    Serial.print("Using Server: "); Serial.print(iothub_server); Serial.print(" Port: "); Serial.println(iothub_port);

    RegisterRouteHandlers();
  }

  void ClearEeprom() {
    // clear eeprom
    for (int i = 0 ; i < 256 ; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.commit();
  }

  void StartConfig() {};


  void Send(uint sensor_index,float sensor_value) {
      Serial.print("Sensor "); Serial.print(sensor_index); Serial.print(" value "); Serial.println(sensor_value);

      HTTPClient http;

      // generate the URL for sensor
      String url = "/api/sensors/";
      url.concat(sensor_ids[sensor_index]);
      url.concat("/data");
      Serial.print("Url: "); Serial.println(url);
      // Make a HTTP post
      http.begin(iothub_server,iothub_port, url);

      // prep the json object
      StaticJsonBuffer<50> jsonBuffer;
      JsonObject& json_obj = jsonBuffer.createObject();
      json_obj["value"] = sensor_value;

      http.addHeader("Content-Type", "application/json"); // important! JSON conversion in nodejs requires this

      // add the json to a string
      String json_string;
      json_obj.printTo(json_string);// this is great except it seems to be adding quotation marks around what it is sending
      // then send the json

      Serial.print("Sending Data: "); Serial.println(json_string);
      int http_code = http.POST(json_string);

      // then print the response over Serial
      Serial.print("Response: "); Serial.println( http.getString() );
      Serial.print("HTTP Code: "); Serial.println(http_code);Serial.println();

      if (http_code == 404) {
        // set first boot so that sensors can be registered on next boot
        SetFirstBoot();
        // restart and re-register sensors
        Serial.println("Sensor 404'd restarting");
        ESP.restart();
      }

      http.end();
  };

  // this should post to /api/sensors with the requested sensor, this will then return an ID that can be used
void RegisterSensors(const char* sensor_names[]) {
    ShowEeprom();
    // if first boot
    if ( CheckFirstBoot() ) {
      if(sensor_names[0] == '\0') {
        Serial.println("Sensor name provided to RegisterSensors empty, the sensor name must not be empty");
        return;
      }

      Serial.println("First boot, getting fresh sensor IDs from server");
      // ClearEeprom
      ClearEeprom();
      // register sensors
      for(uint i=0; i < number_sensor_ids;i++ ) {
         RegisterSensor(sensor_names[i],&sensor_ids[i]);
      }
      // save sensor ids to eeprom
      SaveIds();
      // change boot status
      UpdateFirstBoot();
    } else {
      // otherwise load from eeprom
      Serial.println("Not first boot, loading IDs from eeprom");
      LoadIds();
    }
    ShowEeprom();
  };

  void RegisterActor(const char* actor_name ,void (*function_pointer)(int)) {
    // do some validation
    // check actor name not too long
    if (strlen(actor_name) > max_node_name_length) {
      Serial.println("Actor being registered had a name length over that set by max_node_name_length");
      return;
    }
    // check that we don't already have too many actors
    if (last_actor_added_index > number_actor_ids) {
      Serial.println("Actor being registered was more than the number specified in initialisation.");
      return;
    }

    actor new_actor;
    new_actor.name = actor_name;
    new_actor.state_type = actor::is_int;
    new_actor.on_update.icallback = function_pointer;
    BaseRegisterActor(&new_actor);
    actors[last_actor_added_index] = new_actor;
    last_actor_added_index++; // increment last actor added
  }

  void Tick() {
    if (number_actor_ids > 0) {
      CheckConnections();
    }
    // disable wifi while sleeping
    //WiFi.forceSleepBegin();
    else if (number_sensor_ids > 0) {
      delay(sleep_interval); // note that delay has built in calls to yeild() :)
    }

    //unsigned long time_wifi_starting = millis();
    // re-enble wifi after sleeping
    //WiFi.forceSleepWake();

    //Serial.print("Wifi status: "); Serial.println(WiFi.status());

    //unsigned long time_wifi_started = millis();
    //Serial.print("Time taken to reconnect to wifi: "); Serial.println( time_wifi_started - time_wifi_starting );
  }
};
