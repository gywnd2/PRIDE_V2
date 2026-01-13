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
  displayMgr.Println("Display Initialized");

  storageMgr.Init();
  storageMgr.ScanDirectory("/", 0);
  displayMgr.Println("Storage Scanned");



  displayMgr.Println("Setup Completed");
}

void loop() {

}