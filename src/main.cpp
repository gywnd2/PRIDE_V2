#include <Arduino.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>
#include <Mp3Mgr.h>

DisplayMgr displayMgr;
StorageMgr storageMgr;
Mp3Mgr mp3Mgr;

void setup() {
  Serial.begin(115200);
  Serial.println("Setup started");

  mp3Mgr.InitMp3();

  displayMgr.Init();
  displayMgr.BacklightOn();

  displayMgr.Println("ESP32 / Guition JC8048W550_I Start Up");
  displayMgr.Println("ESP-IDF Version : "+String(esp_get_idf_version()));
  displayMgr.Println("Display Initialized");

  storageMgr.Init();
  storageMgr.ScanDirectory("/", 0);
  displayMgr.Println("Storage Scanned");

  GIFMemory splashMem = storageMgr.LoadGifToPSRAM("/anim/splash.gif");
  if(splashMem.size > 0) {
    displayMgr._pendingGifData = splashMem.data;
    displayMgr._pendingGifSize = splashMem.size;
  }

  displayMgr.Println("Setup continue while GIF playing...");
  displayMgr.Println("Setup Completed");
}

void loop() {

}