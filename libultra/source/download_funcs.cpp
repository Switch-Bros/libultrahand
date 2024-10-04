#include "download_funcs.hpp"

namespace ult {
    size_t DOWNLOAD_BUFFER_SIZE = 4096 * 4;
    size_t UNZIP_BUFFER_SIZE = 4096 * 4;

    // Static variables definition
    const std::string cacertPath = "sdmc:/config/ultrahand/cacert.pem";
    const std::string cacertURL = "https://curl.se/ca/cacert.pem";
    std::atomic<bool> abortDownload(false);
    std::atomic<bool> abortUnzip(false);
    std::atomic<int> downloadPercentage(-1);
    std::atomic<int> unzipPercentage(-1);
    const std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36";
    
    void CurlDeleter::operator()(CURL* curl) const {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
    
    size_t writeCallback(void* ptr, size_t size, size_t nmemb, std::ostream* stream) {
        auto& file = *static_cast<std::ofstream*>(stream);
        size_t totalBytes = size * nmemb;
        file.write(static_cast<const char*>(ptr), totalBytes);
        return totalBytes;
    }
    
    extern "C" int progressCallback(void* ptr, curl_off_t totalToDownload, curl_off_t nowDownloaded, curl_off_t totalToUpload, curl_off_t nowUploaded) {
        auto percentage = static_cast<std::atomic<int>*>(ptr);
        if (totalToDownload > 0) {
            percentage->store(static_cast<int>(float(nowDownloaded) / float(totalToDownload) * 100), std::memory_order_release);
        }
        if (abortDownload.load(std::memory_order_acquire)) {
            percentage->store(-1, std::memory_order_release);
            return 1;  // Abort the download
        }
        return 0;  // Continue the download
    }
    
    void initializeCurl() {
        CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (res != CURLE_OK) {
            logMessage("curl_global_init() failed: " + std::string(curl_easy_strerror(res)));
        }
    }
    
    void cleanupCurl() {
        curl_global_cleanup();
    }
    
    bool downloadFile(const std::string& url, const std::string& toDestination) {
        abortDownload.store(false);
        
        if (url.find_first_of("{}") != std::string::npos) {
            logMessage("Invalid URL: " + url);
            return false;
        }
    
        std::string destination = toDestination;
        if (destination.back() == '/') {
            createDirectory(destination);
            size_t lastSlash = url.find_last_of('/');
            if (lastSlash != std::string::npos) {
                destination += url.substr(lastSlash + 1);
            } else {
                logMessage("Invalid URL: " + url);
                return false;
            }
        } else {
            createDirectory(destination.substr(0, destination.find_last_of('/')));
        }
    
        if (!isDirectory(DOWNLOADS_PATH)) {
            createDirectory(DOWNLOADS_PATH);
        }
    
        std::string tempFilePath = DOWNLOADS_PATH + getFileName(destination) + ".tmp";
        std::ofstream file(tempFilePath, std::ios::binary);
        if (!file.is_open()) {
            logMessage("Error opening file: " + tempFilePath);
            return false;
        }
    
        std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
        if (!curl) {
            logMessage("Error initializing curl.");
            file.close();
            return false;
        }
    
        downloadPercentage.store(0, std::memory_order_release);
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &downloadPercentage);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, cacertPath.c_str()); // Set CA certificate path

        CURLcode res = curl_easy_perform(curl.get());
        if (res != CURLE_OK) {
            logMessage("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
            file.close();
            return false;
        }

        // Close the file after download
        file.close();

        // Rename the temp file to the final destination
        std::rename(tempFilePath.c_str(), destination.c_str());
        return true;
    }

    /**
     * @brief Extracts files from a ZIP archive to a specified destination.
     *
     * @param zipFilePath The path to the ZIP archive file.
     * @param toDestination The destination directory where files should be extracted.
     * @return True if the extraction was successful, false otherwise.
     */
    bool unzipFile(const std::string& zipFilePath, const std::string& toDestination) {
        abortUnzip.store(false, std::memory_order_release); // Reset abort flag
    
        std::unique_ptr<ZZIP_DIR, ZzipDirDeleter> dir(zzip_dir_open(zipFilePath.c_str(), nullptr));
        if (!dir) {
            logMessage("Error opening zip file: " + zipFilePath);
            return false;
        }
    
        ZZIP_DIRENT entry;
        zzip_ssize_t totalUncompressedSize = 0;
    
        // First pass: Calculate the total size of all files
        while (zzip_dir_read(dir.get(), &entry)) {
            if (entry.d_name[0] != '\0' && entry.st_size > 0) {
                totalUncompressedSize += entry.st_size;
            }
        }
    
        // Close and reopen the directory for extraction
        dir.reset(zzip_dir_open(zipFilePath.c_str(), nullptr));
        if (!dir) {
            logMessage("Failed to reopen zip file: " + zipFilePath);
            return false;
        }
    
        bool success = true;
        zzip_ssize_t currentUncompressedSize = 0;
    
        std::string fileName, extractedFilePath;
        std::string directoryPath;
        char buffer[UNZIP_BUFFER_SIZE];
        zzip_ssize_t bytesRead;
    
        unzipPercentage.store(0, std::memory_order_release); // Initialize percentage
    
        std::unique_ptr<ZZIP_FILE, ZzipFileDeleter> file;
        std::ofstream outputFile;
    
        auto it = extractedFilePath.end();
        // Second pass: Extract files and update progress
        while (zzip_dir_read(dir.get(), &entry)) {
            if (entry.d_name[0] == '\0') continue; // Skip empty entries
    
            fileName = entry.d_name;
    
            // Remove invalid characters
            extractedFilePath = toDestination + fileName;
            it = extractedFilePath.begin() + std::min(extractedFilePath.find(ROOT_PATH) + 5, extractedFilePath.size());
            extractedFilePath.erase(std::remove_if(it, extractedFilePath.end(), [](char c) {
                return c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|';
            }), extractedFilePath.end());
    
            if (!extractedFilePath.empty() && extractedFilePath.back() == '/') continue; // Skip directories
    
            directoryPath = extractedFilePath.substr(0, extractedFilePath.find_last_of('/') + 1);
            createDirectory(directoryPath);
    
            // Reset the unique_ptr before opening a new file
            file.reset(zzip_file_open(dir.get(), entry.d_name, 0));
            if (!file) {
                logMessage("Error opening file in zip: " + fileName);
                success = false;
                continue;
            }
    
            outputFile.open(extractedFilePath, std::ios::binary);
            if (!outputFile.is_open()) {
                logMessage("Error opening output file: " + extractedFilePath);
                success = false;
                continue;
            }
    
            while ((bytesRead = zzip_file_read(file.get(), buffer, UNZIP_BUFFER_SIZE)) > 0) {
                if (abortUnzip.load(std::memory_order_acquire)) {
                    logMessage("Aborting unzip operation during file extraction.");
                    outputFile.close();
                    deleteFileOrDirectory(extractedFilePath); // Cleanup partial file
                    success = false;
                    break;
                }
                outputFile.write(buffer, bytesRead);
                logMessage("Extracted: " + extractedFilePath);
    
                currentUncompressedSize += bytesRead;
    
                unzipPercentage.store(totalUncompressedSize != 0 ?
                    static_cast<int>(100.0 * std::min(1.0, static_cast<double>(currentUncompressedSize) / static_cast<double>(totalUncompressedSize))) : 0,
                    std::memory_order_release);
            }
            outputFile.close();
    
            if (!success) break; // Exit the outer loop if interrupted during extraction
        }
    
        if (success) {
            unzipPercentage.store(100, std::memory_order_release); // Ensure it's set to 100% on successful extraction
            //logMessage("Extraction Complete!");
        } else {
            unzipPercentage.store(-1, std::memory_order_release);
        }
    
        return success;
    }
}