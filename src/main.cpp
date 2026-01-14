#include <Arduino.h>
#include <DisplayMgr.h>
#include <StorageMgr.h>

DisplayMgr displayMgr;
StorageMgr storageMgr;

void setup() {
  Serial.begin(115200);
  Serial.println("Setup started");

  displayMgr.Init();
  displayMgr.BacklightOn();

  displayMgr.Println("ESP32 / Guition JC8048W550_I Start Up");
  displayMgr.Println("ESP-IDF Version : "+String(esp_get_idf_version()));
  displayMgr.Println("Display Initialized");

  storageMgr.Init();
  storageMgr.ScanDirectory("/", 0);
  displayMgr.Println("Storage Scanned");

  GIFMemory splashMem = storageMgr.LoadGifToPSRAM("/anim/splash.gif");
  if(splashMem.size > 0)
  {
    displayMgr.Println("Load splash anim successful");
  }

  if(storageMgr.SDExists("/anim/splash.gif"))
  {
    displayMgr.Clear();
    displayMgr.PlayGifFromMemory(splashMem.data, splashMem.size, false);
  }

  displayMgr.Println("Setup Completed");
}

void loop() {

}