
  #include <MFRC522v2.h>
  #include <MFRC522DriverSPI.h>
  #include <MFRC522DriverPinSimple.h>
  #include <MFRC522Debug.h>

  #include <WiFi.h>                  // Thư viện WiFi
  #include <Firebase_ESP_Client.h>   // Thư viện Firebase
  #include <addons/TokenHelper.h>    // Helper cho token
  #include <addons/RTDBHelper.h>     // Helper cho RTDB

  #include <time.h>                  // Thư viện time cho configTime và getLocalTime

  #include <Wire.h>                  // For I2C
  #include <LiquidCrystal_I2C.h>     // For LCD I2C

  #include <Keypad.h>                // Thư viện Keypad

  #include <ESP32Servo.h>            // Thư viện cho Servo trên ESP32

  // Định nghĩa pin RFID
  MFRC522DriverPinSimple ss_pin(5);  // Chân SS/SDA
  MFRC522DriverSPI driver{ss_pin};   // Driver SPI
  MFRC522 mfrc522{driver};           // Instance MFRC522

  // WiFi credentials (thay bằng của bạn)
  #define WIFI_SSID "Hoangpro"
  #define WIFI_PASSWORD "12345678"

  // Firebase credentials (thay bằng của bạn)
  #define API_KEY "AIzaSyAYEnyFrWA-smpNX_2vJZlwgZtdBi8RqP0"
  #define DATABASE_URL "https://rfid-a5c39-default-rtdb.firebaseio.com/"  // Ví dụ: https://your-project.firebaseio.com

  // Firebase objects
  FirebaseData fbdo;
  FirebaseData stream;
  FirebaseAuth auth;
  FirebaseConfig config;

  // LCD I2C (adjust address if different, e.g., 0x3F)
  #define LCD_ADDRESS 0x27
  #define LCD_COLUMNS 16
  #define LCD_ROWS 2
  LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

  // Keypad 4x4
  const byte ROWS = 4; // 4 hàng
  const byte COLS = 4; // 4 cột
  char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
  };
  byte rowPins[ROWS] = {13, 12, 14, 27}; // Kết nối hàng (thay nếu cần)
  byte colPins[COLS] = {26, 25, 33, 32}; // Kết nối cột (thay nếu cần)
  Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

  // Biến cho nhập mật khẩu
  bool enteringPassword = false;
  String password = "";
  String bossPassword = "1234";  // Ban đầu default, sẽ update từ Firebase
  const String bossUID = "f4f7a605";  // UID của boss (dùng để log)

  // Servo
  Servo myservo;  // Instance servo
  #define SERVO_PIN 16  // GPIO16 cho PWM

  // Hàm lấy user key từ UID (tra cứu từ /rfid_to_user/UID)
  String getUserKeyFromUID(const String& uid) {
    String path = "/rfid_to_user/" + uid;
    if (Firebase.RTDB.getString(&fbdo, path)) {
      String userKey = fbdo.stringData();
      if (userKey != "") {
        return userKey;
      }
    } else {
      Serial.println("No mapping found for UID: " + uid);
    }
    return "";
  }

  // Hàm tạo user mới tự động và trả về userKey mới (tìm số nhỏ nhất chưa tồn tại)
  String createNewUser(const String& uid) {
    int nextNum = 1;
    while (true) {
      String testKey = "user" + String(nextNum);
      if (!Firebase.RTDB.pathExisted(&fbdo, "/uid/" + testKey)) {
        String newUserKey = testKey;     
        // Set fields default với access="deny"
        setField(newUserKey, "name", "New User " + String(nextNum));
        setField(newUserKey, "date", "2000-01-01");
        setField(newUserKey, "access", "deny");
        setField(newUserKey, "last_visit_in", "null");
        setField(newUserKey, "last_visit_out", "null");
        setField(newUserKey, "type", "user");
        setField(newUserKey, "to_delete", "false");  // Default không xóa
        
        // Set mapping /rfid_to_user/UID = newUserKey
        if (!setFieldRfidToUser(uid, newUserKey)) {
          return "";
        }
        
        Serial.println("Created new user: " + newUserKey + " with UID: " + uid + " (access=deny)");
        return newUserKey;
      }
      nextNum++;
    }
  }

  // Hàm xóa user và mapping UID
  void deleteUser(const String& userKey) {
    // Tìm và xóa tất cả mapping /rfid_to_user/UID nếu val == userKey
    if (Firebase.RTDB.getJSON(&fbdo, "/rfid_to_user")) {
      FirebaseJson &json = fbdo.jsonObject();
      size_t len = json.iteratorBegin();
      String key, val;
      int type;
      bool found = false;
      for (size_t i = 0; i < len; i++) {
        json.iteratorGet(i, type, key, val);
        if (val == userKey) {
          String mapPath = "/rfid_to_user/" + key;
          if (Firebase.RTDB.deleteNode(&fbdo, mapPath)) {
            Serial.println("Deleted rfid_to_user mapping for UID: " + key);
          } else {
            Serial.println("Failed to delete rfid_to_user for UID: " + key + ": " + fbdo.errorReason());
          }
          found = true;
          // Không break để xóa tất cả mapping match
        }
      }
      json.iteratorEnd();
      if (!found) {
        Serial.println("No mapping found for user: " + userKey);
      }
    } else {
      Serial.println("Failed to get /rfid_to_user for delete: " + fbdo.errorReason());
    }
    
    // Xóa node /uid/userKey
    String path = "/uid/" + userKey;
    if (Firebase.RTDB.deleteNode(&fbdo, path)) {
      Serial.println("Deleted user: " + userKey);
    } else {
      Serial.println("Failed to delete user " + userKey + ": " + fbdo.errorReason());
    }
  }

  // Callback cho stream
  void streamCallback(FirebaseStream data) {
    Serial.println("Stream data changed!");
    String path = data.dataPath();
    String value = data.stringData();
    
    Serial.print("Updated at ");
    Serial.print(path);
    Serial.print(": ");
    Serial.println(value);
    
    // Kiểm tra nếu là /userX/to_delete và value == "true", thì xóa user
    if (path.endsWith("/to_delete") && value == "true") {
      // Extract userKey từ path (ví dụ /user3/to_delete -> user3)
      int lastSlash = path.lastIndexOf('/');
      String userKey = path.substring(1, lastSlash);
      if (userKey.startsWith("user")) {
        Serial.println("Detected to_delete=true for " + userKey + ". Deleting...");
        deleteUser(userKey);
      }
    }
    
    // Update bossPassword nếu thay đổi /boss/password
    if (path == "/boss/password") {
      bossPassword = value;
      Serial.println("Updated boss password from Firebase: " + bossPassword);
    }
  }

  void streamTimeoutCallback(bool timeout) {
    if (timeout) {
      Serial.println("Stream timeout, resuming...");
    }
  }

  // Hàm set field
  bool setField(String userKey, String field, String value) {
    String path = "/uid/" + userKey + "/" + field;
    if (Firebase.RTDB.setString(&fbdo, path, value)) {
      Serial.println("Set " + field + " successfully for " + userKey + ": " + value);
      return true;
    } else {
      Serial.println("Failed to set " + field + ": " + fbdo.errorReason());
      return false;
    }
  }

  // Hàm set /rfid_to_user/UID = userKey
  bool setFieldRfidToUser(String uid, String userKey) {
    String path = "/rfid_to_user/" + uid;
    if (Firebase.RTDB.setString(&fbdo, path, userKey)) {
      Serial.println("Set rfid_to_user for " + uid + " to " + userKey);
      return true;
    } else {
      Serial.println("Failed to set rfid_to_user for " + uid + ": " + fbdo.errorReason());
      return false;
    }
  }

  // Hàm xóa mapping UID
  bool deleteMapping(const String& uid) {
    String path = "/rfid_to_user/" + uid;
    if (Firebase.RTDB.deleteNode(&fbdo, path)) {
      Serial.println("Deleted orphan mapping for UID: " + uid);
      return true;
    } else {
      Serial.println("Failed to delete orphan mapping for UID: " + uid + ": " + fbdo.errorReason());
      return false;
    }
  }

  // Hàm get và in tất cả fields
  void printAllFields(String userKey) {
    String fields[] = {"name", "date", "access", "text_to_lcd", "last_visit_in", "last_visit_out", "type", "to_delete"};
    Serial.println("Fields for " + userKey + ":");
    
    for (String field : fields) {
      if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/" + field)) {
        String value = fbdo.stringData();
        Serial.print(field + ": ");
        Serial.println(value);
      } else {
        Serial.print(field + ": ");
        Serial.println("null");
      }
    }
  }

  // Hàm lấy thời gian hiện tại (sử dụng getLocalTime cho múi giờ UTC+7)
  String getCurrentTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      Serial.println("Failed to obtain time");
      return "Time Error";
    }
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
  }

  // Hàm hiển thị trên LCD và xóa sau 5 giây
  void displayOnLCD(const String& line1, const String& line2) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1.substring(0, LCD_COLUMNS));  // Cắt nếu dài hơn 16 ký tự
    lcd.setCursor(0, 1);
    lcd.print(line2.substring(0, LCD_COLUMNS));
    delay(5000);  // Hiển thị 5 giây
    lcd.clear();  // Xóa màn hình sau đó
  }

  // Hàm xử lý toggle và log chung (dùng cho cả RFID và keypad)
  void processAccess(const String& uidString, const String& userKey) {
    if (userKey != "") {
      // Lấy name, type, access
      String name = "Unknown";
      String type = "Unknown";
      String access = "deny";
      if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/name")) {
        name = fbdo.stringData();
      }
      if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/type")) {
        type = fbdo.stringData();
      }
      if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/access")) {
        access = fbdo.stringData();
      }

      String currentTime = getCurrentTime();
      if (currentTime == "Time Error") {
        return;  // Không xử lý nếu thời gian lỗi
      }
      String result = "deny";
      String action = "";
      String lcdLine1 = "";
      String lcdLine2 = currentTime;  // Dòng 2 luôn là thời gian

      if (access == "allow") {
        // Lấy last_visit_in và last_visit_out
        String lastIn = "null";
        String lastOut = "null";
        if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/last_visit_in")) {
          lastIn = fbdo.stringData();
        }
        if (Firebase.RTDB.getString(&fbdo, "/uid/" + userKey + "/last_visit_out")) {
          lastOut = fbdo.stringData();
        }
        
        // Toggle: Nếu out mới hơn hoặc in null, thì in; ngược lại out
        if (lastIn == "null" || (lastOut != "null" && lastOut > lastIn)) {
          setField(userKey, "last_visit_in", currentTime);
          action = "Checked in at " + currentTime;
          result = "allow-in";
          lcdLine1 = "Welcome " + name;
        } else {
          setField(userKey, "last_visit_out", currentTime);
          action = "Checked out at " + currentTime;
          result = "allow-out";
          lcdLine1 = "See you again " + name;
        }
        
        Serial.println(action);
      } else {
        Serial.println("Access denied for UID: " + uidString + " (access=deny)");
        result = "deny";
        lcdLine1 = "Access Denied";
      }

      // Hiển thị trên LCD
      displayOnLCD(lcdLine1, lcdLine2);

      // Kích hoạt servo nếu access allow (cả in và out đều xoay 0->180 giữ 10s ->0)
      if (access == "allow") {
        myservo.write(180);  // Mở cửa
        delay(10000);        // Giữ 10 giây
        myservo.write(0);    // Đóng cửa
      }

      // Push log entry vào /log
      FirebaseJson logJson;
      logJson.add("time", currentTime);
      logJson.add("uid", uidString);
      logJson.add("name", name);
      logJson.add("type", type);
      logJson.add("result", result);

      if (Firebase.RTDB.pushJSON(&fbdo, "/log", &logJson)) {
        Serial.println("Logged access: " + result + " for UID: " + uidString + " at " + currentTime);
      } else {
        Serial.println("Failed to log access: " + fbdo.errorReason());
      }

      printAllFields(userKey);
    } else {
      Serial.println("Failed to create or find user for UID: " + uidString);
    }
  }

  void setup() {
    Serial.begin(115200);
    while (!Serial);
    
    // Initialize keypad pins explicitly
    for (byte r = 0; r < ROWS; r++) {
      pinMode(rowPins[r], INPUT_PULLUP);
    }
    for (byte c = 0; c < COLS; c++) {
      pinMode(colPins[c], OUTPUT);
      digitalWrite(colPins[c], HIGH);
    }
    
    // Re-initialize keypad if necessary
    keypad.begin(makeKeymap(keys));
    keypad.setDebounceTime(50);  // Set debounce time to 50ms
    
    // Kết nối WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.println("\nConnected to WiFi");
    
    // Khởi tạo thời gian với múi giờ UTC+7
    Serial.println("Configuring time with NTP...");
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    // Chờ đồng bộ thời gian (tối đa 5 giây)
    struct tm timeinfo;
    unsigned long start = millis();
    while (!getLocalTime(&timeinfo) && (millis() - start < 5000)) {
      delay(500);
      Serial.print(".");
    }
    if (getLocalTime(&timeinfo)) {
      Serial.println("\nTime synchronized");
    } else {
      Serial.println("\nFailed to synchronize time");
    }
    
    // Khởi tạo Firebase
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    config.token_status_callback = tokenStatusCallback;
    
    Serial.print("Anonymous sign up... ");
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("OK");
      Serial.print("User UID: ");
      Serial.println(auth.token.uid.c_str());
    } else {
      Serial.printf("Failed, %s\n", config.signer.signupError.message.c_str());
    }
    
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    
    // Set giá trị ban đầu cho boss nếu chưa tồn tại
    if (!Firebase.RTDB.pathExisted(&fbdo, "/uid/boss")) {
      setField("boss", "name", "Boss Default Name");
      setField("boss", "date", "1980-01-01");
      setField("boss", "access", "allow");
      setField("boss", "last_visit_in", "null");
      setField("boss", "last_visit_out", "null");
      setField("boss", "type", "boss");
      setField("boss", "to_delete", "false");
    }
    // Set mapping cho boss nếu chưa có
    String uidBoss = "f4f7a605";
    if (getUserKeyFromUID(uidBoss) == "") {
      setFieldRfidToUser(uidBoss, "boss");
    }
    
    // Set default password cho boss nếu chưa tồn tại
    if (!Firebase.RTDB.pathExisted(&fbdo, "/uid/boss/password")) {
      setField("boss", "password", "1234");
    }
    // Lấy password từ Firebase
    if (Firebase.RTDB.getString(&fbdo, "/uid/boss/password")) {
      bossPassword = fbdo.stringData();
      Serial.println("Loaded boss password from Firebase: " + bossPassword);
    } else {
      Serial.println("Failed to load boss password: " + fbdo.errorReason());
    }
    
    // Set giá trị ban đầu cho user1 nếu chưa tồn tại
    if (!Firebase.RTDB.pathExisted(&fbdo, "/uid/user1")) {
      setField("user1", "name", "User1 Default Name");
      setField("user1", "date", "1990-01-01");
      setField("user1", "access", "allow");
      setField("user1", "last_visit_in", "null");
      setField("user1", "last_visit_out", "null");
      setField("user1", "type", "user");
      setField("user1", "to_delete", "false");
      String uidUser1 = "f5349305";
      setFieldRfidToUser(uidUser1, "user1");
    }
    
    // Xác nhận
    Serial.println("Confirming fields from Firebase:");
    printAllFields("boss");
    printAllFields("user1");
    
    // Setup stream
    if (!Firebase.RTDB.beginStream(&stream, "/uid")) {
      Serial.println("Could not begin stream: " + stream.errorReason());
    }
    Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);
    
    // Khởi tạo RFID
    mfrc522.PCD_Init();
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
    Serial.println(F("Quét thẻ RFID để đọc UID"));
    Serial.println(F("Để xóa user qua Serial (debug): gửi 'delete userX' (ví dụ: delete user2)"));
    Serial.println(F("Để xóa user thực tế: set /uid/userX/to_delete = \"true\" trên Firebase console"));
    Serial.println(F("Để nhập mật khẩu boss: Ấn '*' rồi nhập 1234 trên keypad"));
    
    // Khởi tạo LCD
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.print("RFID System Ready");
    delay(2000);  // Hiển thị thông báo khởi động 2 giây
    lcd.clear();

    // Khởi tạo Servo
    myservo.attach(SERVO_PIN);
    myservo.write(0);  // Vị trí ban đầu đóng cửa
  }

  void loop() {
    // Xử lý lệnh từ Serial (xóa user - cho debug/demo)
    if (Serial.available() > 0) {
      String command = Serial.readStringUntil('\n');
      command.trim();
      if (command.startsWith("delete ")) {
        String userKeyToDelete = command.substring(7);
        userKeyToDelete.trim();
        if (userKeyToDelete.startsWith("user")) {
          deleteUser(userKeyToDelete);
        } else {
          Serial.println("Invalid user key: " + userKeyToDelete + ". Must be 'userX'");
        }
      }
    }

    // Xử lý keypad
    char key = keypad.getKey();
    if (key) {
      Serial.print("Key pressed: ");
      Serial.println(key);
      
      if (!enteringPassword && key == '*') {
        enteringPassword = true;
        password = "";
        lcd.clear();
        lcd.print("Enter Password:");
        Serial.println("Entering password mode");
      } else if (enteringPassword) {
        password += key;
        lcd.setCursor(password.length() - 1, 1);
        lcd.print('*');  // Hiển thị '*' cho mỗi ký tự
        
        if (password.length() == 4) {
          if (password == bossPassword) {
            Serial.println("Boss password correct. Processing as boss.");
            String userKey = "boss";
            processAccess(bossUID, userKey);  // Xử lý như quét UID boss
          } else {
            Serial.println("Incorrect password.");
            displayOnLCD("Wrong Password", "");
          }
          enteringPassword = false;
          password = "";
          delay(2000);
          lcd.clear();
        }
      }
    }

    // Xử lý RFID
    if (!mfrc522.PICC_IsNewCardPresent()) {
      return;
    }

    if (!mfrc522.PICC_ReadCardSerial()) {
      return;
    }

    String uidString = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) {
        uidString += "0";
      }
      uidString += String(mfrc522.uid.uidByte[i], HEX);
    }
    Serial.print("UID thẻ: ");
    Serial.println(uidString);

    String userKey = getUserKeyFromUID(uidString);
    if (userKey != "") {
      // Check nếu node /uid/userKey tồn tại, nếu không thì mapping sót, xóa và tạo mới
      if (!Firebase.RTDB.pathExisted(&fbdo, "/uid/" + userKey)) {
        Serial.println("Orphan mapping detected for " + userKey + ". Deleting mapping and creating new user.");
        deleteMapping(uidString);
        userKey = "";
      }
    }

    if (userKey == "") {
      // Tạo user mới nếu không tồn tại
      userKey = createNewUser(uidString);
    }

    processAccess(uidString, userKey);

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }