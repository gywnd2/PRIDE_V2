#include <StorageMgr.h>
#include <CommonApi.h>

void StorageMgr::Init()
{
    if(!SD.begin(TF_CS, SPI, 40000000))
    {
        Serial.println("[StorageMgr] SD Card Mount Failed");
    }
    else
    {
        Serial.println("[StorageMgr] SD Card Mounted Successfully");
        this->ScanDirectory("/", 0);

        SystemAPI* system = SystemAPI::getInstance();
        if (system->LockGif(pdMS_TO_TICKS(500))) {
            GIFMemory* objPtr = system->GetPsramObjPtr();
            if (objPtr) {
                if (objPtr->data != nullptr) {
                    heap_caps_free(objPtr->data);
                    objPtr->data = nullptr;
                    objPtr->size = 0;
                }
                *objPtr = this->LoadGifToPSRAM("/anim/splash.gif");
                system->isGifLoaded = (objPtr->data != nullptr);
                Serial.printf("[StorageMgr] Splash GIF preload: %s (%u bytes)\n",
                              system->isGifLoaded ? "OK" : "FAIL",
                              (unsigned int)objPtr->size);
            }
            system->UnlockGif();
        } else {
            Serial.println("[StorageMgr] Splash GIF preload skipped (gif mutex timeout)");
        }

        if (taskHandler != NULL) {
            vTaskDelete(taskHandler);
            taskHandler = NULL;
        }

        xTaskCreate(
            StorageMgr::Subscribe,
            "StorageEventSubscriber",
            8192,
            this,
            1,
            &taskHandler
        );
    }
}

void StorageMgr::Subscribe(void* pvParameters)
{
    StorageMgr* self = static_cast<StorageMgr*>(pvParameters);
    SystemAPI* system = SystemAPI::getInstance();
    StorageEventSubscriber& subscriber = system->storageSubscriber;

    StorageEventData event;

    while (true)
    {
        if(subscriber.ReceiveEvent(&event, portMAX_DELAY))
        {
            Serial.printf("[StorageMgr] Subscribe: %d\n", event.type);
            switch(event.type)
            {
                case STORAGE_EVENT_TYPE::STORAGE_EVENT_NONE:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE:: STORAGE_SCAN:
                {
                    self->ScanDirectory("/", 0);
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_READ:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_WRITE:
                {
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_LOAD_TO_PSRAM:
                {
                    String filePath = String(event.filePath);
                    GIFMemory* objPtr = system->GetPsramObjPtr();
                    if(objPtr == nullptr) {
                        Serial.println("[StorageMgr] Critical Error: gifObj pointer is NULL");
                        break;
                    }

                    if (system->LockGif()) {
                        if(objPtr->data != nullptr) {
                            heap_caps_free(objPtr->data);
                            objPtr->data = nullptr;
                            objPtr->size = 0;
                        }

                        *objPtr = self->LoadGifToPSRAM(filePath.c_str());
                        system->isGifLoaded = (objPtr->data != nullptr);
                        system->UnlockGif();
                        Serial.println("[StorageMgr] Successfully loaded gif to PSRAM");
                    } else {
                        Serial.println("[StorageMgr] Failed to acquire GIF lock for loading");
                    }
                    break;
                }
                case STORAGE_EVENT_TYPE::STORAGE_CLEAR_LOADED_PSRAM:
                {
                    GIFMemory* objPtr = system->GetPsramObjPtr();
                    if (system->LockGif()) {
                        if(objPtr && objPtr->data != nullptr) {
                            heap_caps_free(objPtr->data);
                            objPtr->data = nullptr;
                            objPtr->size = 0;
                        }
                        system->isGifLoaded = false;
                        system->UnlockGif();
                        Serial.println("[StorageMgr] Cleared gif in PSRAM");
                    } else {
                        Serial.println("[StorageMgr] Failed to acquire GIF lock for clearing");
                    }
                    break;
                }
                default:
                    Serial.printf("[StorageMgr] Subscribe: Wrong Event type : %d\n", event.type);
                    break;
            }
        }
    }
}


void StorageMgr::ScanDirectory(const char* path, uint8_t depth)
{
    File dir = SD.open(path);
    if(!dir || !dir.isDirectory()) return;

    File entry;
    while(true)
    {
        entry = dir.openNextFile();
        if(!entry) break;

        FileNode node;
        node.fileName = String(entry.name());
        node.isDirectory = entry.isDirectory();
        node.depth = depth;

        this->fileList.push_back(node);
        if (entry.isDirectory())
        {
            Serial.printf("[StorageMgr] Dir: %s Depth: %d\n", entry.name(), depth);

            if (String(entry.name()) != String("System Volume Information"))
            {
                String childPath = String(path);
                if (!childPath.endsWith("/")) childPath += "/";
                childPath += entry.name();

                this->ScanDirectory(childPath.c_str(), depth + 1);
            }
            else
            {
                Serial.println("[StorageMgr] Skipping \"System Volume Information\"");
            }
        }
        else
        {
            Serial.printf("[StorageMgr] File: %s Depth: %d Size: %u bytes\n", entry.name(), depth, entry.size());
        }
        entry.close();
    }
    dir.close();
}

bool StorageMgr::SDExists(const char* path)
{
    return SD.exists(path);
}

File StorageMgr::SDOpen(const char* path, const char* mode)
{
    if (mode && strcmp(mode, "w") == 0)
    {
        return SD.open(path, FILE_WRITE);
    }
    return SD.open(path);
}

size_t StorageMgr::SDReadAll(const char* path, uint8_t* buffer, size_t maxLen)
{
    File f = SD.open(path);
    if (!f) return 0;
    size_t read = 0;
    while (f.available() && read < maxLen)
    {
        int r = f.read(&buffer[read], (int)min((size_t)255, maxLen - read));
        if (r <= 0) break;
        read += r;
    }
    f.close();
    return read;
}

bool StorageMgr::SDRemove(const char* path)
{
    return SD.remove(path);
}

GIFMemory StorageMgr::LoadGifToPSRAM(const char* path) {
    GIFMemory mem;

    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Cannot open %s\n", path);
        return mem;
    }

    size_t fileSize = f.size();
    mem.data = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem.data == nullptr) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Failed to allocate %u bytes\n", (unsigned int)fileSize);
        f.close();
        return mem;
    }

    size_t bytesRead = f.read(mem.data, fileSize);
    f.close();

    if (bytesRead != fileSize) {
        Serial.printf("[StorageMgr] Load PSRAM Error: Read mismatch (%u/%u)\n",
                      (unsigned int)bytesRead, (unsigned int)fileSize);
        heap_caps_free(mem.data);
        mem.data = nullptr;
        return mem;
    }

    mem.size = fileSize;
    return mem;
}

void StorageMgr::FreeGifFromPSRAM(GIFMemory& mem) {
    if (mem.data != nullptr) {
        heap_caps_free(mem.data);
        mem.data = nullptr;
        mem.size = 0;
    }
}
