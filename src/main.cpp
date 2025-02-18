#include "main.h"
#include "config.h"
#include "time.h"
#include "esp_sntp.h"
#include <ESP32Servo.h>
#include <WiFi.h>
#include "camera_pins.h"
// #include "soc/rtc.h"

const char *ntp_server1 = "pool.ntp.org";
const char *ntp_server2 = "time.nist.gov";
const char *ntp_server3 = "cn.ntp.org.cn";
// const char *timezone = "HKT-8";
const long gmtOffset_sec = 8 * 60 * 60;
const int daylightOffset_sec = 0;

bool camera_is_inited = false, isStreaming = false;
uint8_t flash_br = 0;
String uart0_rbuf = "";
time_t ts, ts_camera_open;
tm struct_ts;

camera_config_t config;
HardwareSerial uart0 = Serial;
// Preallocate the 1st and 2nd PWM channel and overwrite with camera,
// in order to avoid channel conflict
//Servo s_prealloc0;
//Servo s_prealloc1;
Servo s_pitch;
Servo s_yaw;

#if SET_WIREGUARD_ENABLE
#include <WireGuard-ESP32.h>

WireGuard wg;
#endif

char hostmsg[256];
char *hostamsg = hostmsg;

int r_wifi = 0;

void setup() {
	// esp_log_level_set("*", ESP_LOG_DEBUG);

	pinMode(LED_BUILTIN, OUTPUT);
	pinMode(LED_FLASH, OUTPUT);

	digitalWrite(LED_BUILTIN, LOW);

	strcpy(hostmsg, hostname);
	hostamsg = hostmsg + strlen(hostmsg);

	uart0.begin(115200);
	uart0.setDebugOutput(true);
	uart0.println();

    uart0.print("Attaching servo: ");
	// s_prealloc0.attach(SERVO_PITCH);
	// s_prealloc1.attach(SERVO_PITCH);
    uart0.printf("pitch at CH%d, ", s_pitch.attach(SERVO_PITCH));
	s_pitch.write(pitch_default);
    uart0.printf("yaw at CH%d. ", s_yaw.attach(SERVO_YAW));
	s_yaw.write(yaw_default);
    uart0.println("done.");
	analogWrite(LED_FLASH, 10);

	uart0.print("Initializing camera module: ");
	esp_err_t err = cam_init();
	if(err == ESP_OK) uart0.println("done.");
	else {
		uart0.printf("failed with error 0x%x", err);
		uart0.println("");
		analogWrite(LED_FLASH, 63);
	}

	// comment the line below if you needn't static IP
#if SET_WIFI_USE_STATIC_IP
    WiFi.config(local_ip, gateway, netmask, IPAddress(223, 5, 5, 5), gateway);
#endif
    WiFi.setAutoConnect(true);
    //WiFi.setAutoReconnect(true);
	//WiFi.persistent(true);
	WiFi.setSleep(false);
    WiFi.setHostname(hostname);
	// TODO: Realize permanent config over serial or hotspot
#ifdef WLAN_UART_CONFIGURABLE
	while(uart0.available()) uart0.print(uart0.read());
	uart0.println("Press any key to interrupt WLAN default config (1s).");
	delay(1000);
	if(uart0.available()) {
		unsigned char _ssid[64] = "", _pass[64] = "";
		while(uart0.available()) uart0.print(uart0.read());
		uart0.print("SSID: ");
		while(_ssid[0] == '\0') {
			while(uart0.available()) {
				char in = uart0.read(); uart0.print(in);
				if(in=='\r' || in=='\n') {
					uart0_rbuf.getBytes(_ssid, uart0_rbuf.length());
					uart0_rbuf.clear();
					break;
				}
				else uart0_rbuf += in;
			}
		}
		while(uart0.available()) uart0.print(uart0.read());
		uart0.print("Password: ");
		while(_pass[0] == '\0') {
			while(uart0.available()) {
				char in = uart0.read(); uart0.print('*');
				if(in=='\r' || in=='\n') {
					uart0_rbuf.getBytes(_pass, uart0_rbuf.length());
					uart0_rbuf.clear();
					break;
				}
				else uart0_rbuf += in;
			}
		}
		while(uart0.available()) uart0.print(uart0.read());
		uart0.print("Connecting to wlreless LAN ");
		uart0.print((char *) _ssid);
		uart0.print(": ");
		WiFi.begin((const char *) _ssid, (const char *) _pass);
	} else {
#endif
		uart0.print("Connecting to wlreless LAN: ");
		uart0.println(ssid);
		// uart0.print(": ");
		// TODO: online config over serial, bluetooth
		WiFi.begin(ssid, password);
#ifdef WLAN_UART_CONFIGURABLE
	}
#endif
	analogWrite(LED_FLASH, 0);
    if(WiFi.waitForConnectResult() != WL_CONNECTED) {
		while(! WiFi.isConnected()) {
			uart0.println("Wifi is not connected.");
			digitalWrite(LED_BUILTIN, LOW);
			delay(500);
			digitalWrite(LED_BUILTIN, HIGH);
			delay(500);
		}
	}
	uart0.println("Wifi connection done.");

	uart0.print("Start sync time: ");
	configTime(gmtOffset_sec, daylightOffset_sec, ntp_server1, ntp_server2, ntp_server3);
	while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
		uart0.println("SNTP is not completed.");
		digitalWrite(LED_BUILTIN, LOW);
		delay(500);
		digitalWrite(LED_BUILTIN, HIGH);
		delay(500);
	}
	if (! getLocalTime(&struct_ts))
		uart0.println("Fail to get local time.");
	uart0.print("SNTP sync done: ");
	uart0.println(&struct_ts, "%B %d %Y %H:%M:%S");

	r_wifi = 10;

	uart0.print("Starting web server: ");
	startCameraServer();
	uart0.print("done, http://");
	uart0.print(WiFi.localIP());
	uart0.println(".");
	time(&ts);
	esp_sleep_enable_timer_wakeup(100e3);

	digitalWrite(LED_BUILTIN, HIGH);
	analogWrite(LED_FLASH, flash_br);
}

void loop() {
	/* Auto sleep after 30s idle
	if(!isStreaming) {
		time(&ts);
		if(ts - ts_camera_open > CAM_IDLE_TIME_MAX) {
			esp_light_sleep_start();
		}
	}
	*/
	/* Auto close camera after 30s idle
	if(camera_is_inited && !isStreaming) {
		time(&ts);
		if(ts - ts_camera_open > CAM_IDLE_TIME_MAX) {
			if(esp_camera_deinit() == ESP_OK) camera_is_inited = false;
		}
	}
	delay(500);
	*/
	if (! WiFi.isConnected() && ! r_wifi) {
		uart0.println("Wifi connection down.");
		uart0.println("Reconnect wifi.");
		WiFi.disconnect();
		WiFi.begin(ssid, password);
		r_wifi = 1500; // Recall WiFi.begin every 5mins;
	}
	if (WiFi.isConnected() && r_wifi) {
		uart0.println("Wifi connection established.");
#if SET_WIREGUARD_ENABLE
		if (! wg.is_initialized()) {
			//uart0.print("Stop old wg connection: ");
			//wg.end();
			//uart0.println("done.");
			// do not need to restart?
			uart0.print("Start wg: ");
			if (wg.begin(
				wg_local_ip,           // IP address of the local interface
				wg_private_key,        // Private key of the local interface
				wg_endpoint_address,   // Address of the endpoint peer.
				wg_public_key,         // Public key of the endpoint peer.
				wg_endpoint_port))     // Port pf the endpoint peer.
				uart0.println("done.");
			else
				uart0.println("initialize failed.");
		}
#endif
		r_wifi = 0;
	}

	if (! WiFi.isConnected() || r_wifi) {
		uart0.printf("Wifi status: %x", WiFi.status());
		uart0.println();
		digitalWrite(LED_BUILTIN, LOW);
		delay(1000);
		digitalWrite(LED_BUILTIN, HIGH);
		delay(1000);
		if (r_wifi > 0) r_wifi--;
	} else {
		delay(10000);
	}
}

esp_err_t cam_init() {
	config.ledc_channel = LEDC_CHANNEL_2;
	config.ledc_timer = LEDC_TIMER_0;
	config.pin_d0 = Y2_GPIO_NUM;
	config.pin_d1 = Y3_GPIO_NUM;
	config.pin_d2 = Y4_GPIO_NUM;
	config.pin_d3 = Y5_GPIO_NUM;
	config.pin_d4 = Y6_GPIO_NUM;
	config.pin_d5 = Y7_GPIO_NUM;
	config.pin_d6 = Y8_GPIO_NUM;
	config.pin_d7 = Y9_GPIO_NUM;
	config.pin_xclk = XCLK_GPIO_NUM;
	config.pin_pclk = PCLK_GPIO_NUM;
	config.pin_vsync = VSYNC_GPIO_NUM;
	config.pin_href = HREF_GPIO_NUM;
	config.pin_sscb_sda = SIOD_GPIO_NUM;
	config.pin_sscb_scl = SIOC_GPIO_NUM;
	config.pin_pwdn = PWDN_GPIO_NUM;
	config.pin_reset = RESET_GPIO_NUM;
	config.xclk_freq_hz = 20000000;
//  config.xclk_freq_hz = 8000000;
	config.pixel_format = PIXFORMAT_JPEG;
	
	/*
	// if PSRAM IC present, init with UXGA resolution and higher JPEG quality
	//                      for larger pre-allocated frame buffer.
	if(psramFound()){
	*/
		config.frame_size = FRAMESIZE_UXGA;
		config.jpeg_quality = 10;
		config.fb_count = 2;
	/*
	} else {
		config.frame_size = FRAMESIZE_SVGA;
		config.jpeg_quality = 12;
		config.fb_count = 1;
	}

#if defined(CAMERA_MODEL_ESP_EYE)
	pinMode(13, INPUT_PULLUP);
	pinMode(14, INPUT_PULLUP);
#endif
	*/

	esp_err_t err = esp_camera_init(&config);
	if (err != ESP_OK) return err;
	sensor_t *s = esp_camera_sensor_get();
	/*
	// initial sensors are flipped vertically and colors are a bit saturated
	if (s->id.PID == OV3660_PID) {
		s->set_vflip(s, 1); // flip it back
		s->set_brightness(s, 1); // up the brightness just a bit
		s->set_saturation(s, -1); // lower the saturation
	}
	*/
	// drop down frame size for higher initial frame rate
	s->set_framesize(s, FRAMESIZE_QVGA);
	s->set_quality(s, 10);
	/*
	// s->set_hmirror(s, 1);
	// s->set_vflip(s, 1);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
	s->set_vflip(s, 1);
	s->set_hmirror(s, 1);
#endif
	*/
	camera_is_inited = true;
	time(&ts_camera_open);
	return ESP_OK;
}

esp_err_t cam_deinit() {
	if(!camera_is_inited) return ESP_ERR_CAMERA_BASE;
	esp_err_t err = esp_camera_deinit();
	return err;
}

esp_err_t cam_reinit() {
	if(camera_is_inited) return ESP_ERR_CAMERA_BASE;
	esp_err_t err = esp_camera_init(&config);
	if (err != ESP_OK) return err;
	sensor_t *s = esp_camera_sensor_get();
	s->set_framesize(s, FRAMESIZE_SVGA);
	s->set_quality(s, 10);
	camera_is_inited = true;
	time(&ts_camera_open);
	return ESP_OK;
}
