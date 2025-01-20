#include <SoftwareSerial.h>
#include <Ticker.h>

#include <FS.h>
#include <time.h>
#include <climits>

class ConsoleLogger {

    Stream& _con;
    Print& _logger;
    FS& _fs;
    fs::File _logFile;
    Ticker _renewTicker;

    // int _checkCount = 0;
    // int _lastCheck = 0;
    int oldestFile = INT_MAX;
    int newestFile = INT_MIN;

    bool dirty = false;

    const int KEEP_SPACE = 1024 * 128;
    const int SINGLE_FILE_SIZE = 1024 * 32;

    void _refreshLogFiles()
    {
        auto dir = _fs.openDir("/log");

        while (dir.next()) {
            String filename = dir.fileName();
            int fileNum = filename.toInt();
            if (fileNum < oldestFile)
                oldestFile = fileNum;
            if (fileNum > newestFile)
                newestFile = fileNum;
        }
    }

    void _printTime()
    {
        struct tm tmstruct;
        time_t now = time(NULL);
        localtime_r(&now, &tmstruct);
        _logFile.printf("\n\n<LogTimeStamp> %d-%02d-%02d %02d:%02d:%02d\n\n", tmstruct.tm_year + 1900, tmstruct.tm_mon + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
    }

public:
    ConsoleLogger(Stream& con, FS& fs, Print& logger)
        : _con(con)
        , _logger(logger)
        , _fs(fs)
        , _logFile(nullptr)
    {
    }

    bool begin(uint16_t autoRenewInterval = 600)
    {
        _refreshLogFiles();
        if (newestFile == INT_MIN)
            newestFile = 0;
        String filename = "/log/" + String(newestFile);
        if (_fs.exists(filename)) {
            auto f = _fs.open(filename, "a+");
            if (f) {
                _logFile = f;
                _logger.print("Open file: ");
                _logger.println(filename);
            } else {
                _logger.print("Failed to open file: ");
                _logger.println(filename);
                return false;
            }
        } else {
            auto f = _fs.open(filename, "w+");
            if (f) {
                _logFile = f;
                _printTime();
                _logFile.close(); // Write to flash and update timestamp!
                _logFile = _fs.open(filename, "a+"); // Open it again, should be fine
                _refreshLogFiles();
                _logger.print("Create file: ");
                _logger.println(filename);
            } else {
                _logger.print("Failed to create file: ");
                _logger.println(filename);
                return false;
            }
        }

        newLogFile();
        _renewTicker.attach(autoRenewInterval, [this]() {
            this->newLogFile();
        });
        return true;
    }

    void end()
    {
        if (_logFile)
            _logFile.close();
        _renewTicker.detach();
    }

    /**
     * @brief new log file according to time and free space
     *
     * @param interval time since last check, in seconds, default to 600s
     */
    void newLogFile()
    {
        bool needRefresh = false;
        // Get FS's free capacity
        FSInfo fs_info;
        _fs.info(fs_info);
        size_t freeSpace = fs_info.totalBytes - fs_info.usedBytes;
        // _logger.printf("FS Total : %u, Used: %u, Free : %u\n", fs_info.totalBytes, fs_info.usedBytes, freeSpace);
        if (freeSpace < KEEP_SPACE) {
            // Try to delete some old files
            // if (oldestFile == INT_MAX) {
            //     _logger.println("No file to delete, please check the FS");
            //     return;
            // }
            String fileToDelete = "/log/" + String(oldestFile);
            if (_logFile && String(_logFile.name()) == fileToDelete) {
                _logger.println("Cannot delete current log file!");
                return;
            }
            needRefresh = true;
            _logger.print("Deleting file: ");
            _logger.print(fileToDelete);
            _logger.print(_fs.remove(fileToDelete) ? " OK!\n" : " Failed!\n");
        }

        if (_logFile.size() > SINGLE_FILE_SIZE) {
            String filename = String(newestFile + 1);
            auto f = _fs.open("/log/" + filename, "w+");
            if (f) {
                _printTime();
                _logger.printf("Closed file: %s, size: %u\n", _logFile.fullName(), _logFile.size());
                _logFile.close(); // will not panic, if not inited
                _logFile = f;
                _printTime();
                _logFile.close(); // Write to flash and update timestamp!
                _logFile = _fs.open("/log/" + filename, "a+"); // Open it again, should be fine
                needRefresh = true;
                dirty = false;
                _logger.print("Open file: ");
            } else {
                _printTime();
                _logFile.flush();
                _logger.print("Failed to create file: ");
            }
            _logger.println(filename);
        } else { // We have file and it's not too big
            if (dirty) {
                _printTime();
                _logFile.flush();
                dirty = false;
            }
        }

        if (needRefresh)
            _refreshLogFiles();
    }

    int doLog(char* buf, int size)
    {
        if (!_logFile) {
            _logger.println("EWC");
            return 0;
        }
        auto x = _con.available();
        if (x > 0) {
            int len = _con.readBytes(buf, x > size ? size : x);
            auto wr = _logFile.write(buf, len);
            dirty = true;
            if (wr != len) {
                _logger.printf("Write failed: %d/%d\n", wr, len);
            }
            // _logger.write(buf, len);
            return len;
        }
        return 0;
    }

    int getMin() const
    {
        return oldestFile;
    }

    int getMax() const
    {
        return newestFile;
    }
};