/********************************************************************************
 * File: download_funcs.cpp
 * Author: ppkantorski
 * Description:
 *   This source file provides implementations for the functions declared in
 *   download_funcs.hpp. These functions utilize libcurl for downloading files
 *   from the internet and minizip-ng for extracting ZIP archives with proper
 *   64-bit file support.
 *
 *   For the latest updates and contributions, visit the project's GitHub repository.
 *   (GitHub Repository: https://github.com/ppkantorski/Ultrahand-Overlay)
 *
 *   Note: Please be aware that this notice cannot be altered or removed. It is a part
 *   of the project's documentation and must remain intact.
 * 
 *  Licensed under both GPLv2 and CC-BY-4.0
 *  Copyright (c) 2024 ppkantorski
 ********************************************************************************/

#include "download_funcs.hpp"


namespace ult {

size_t DOWNLOAD_READ_BUFFER = 4 * 1024;//64 * 1024;//4096*10;
size_t DOWNLOAD_WRITE_BUFFER = 4 * 1024;//64 * 1024;
size_t UNZIP_READ_BUFFER = 4 * 1024;//131072*2;//4096*4;
size_t UNZIP_WRITE_BUFFER = 4 * 1024;//131072*2;//4096*4;


// Path to the CA certificate
const std::string cacertPath = "sdmc:/config/ultrahand/cacert.pem";
const std::string cacertURL = "https://curl.se/ca/cacert.pem";

// Shared atomic flag to indicate whether to abort the download operation
std::atomic<bool> abortDownload(false);
// Define an atomic bool for interpreter completion
std::atomic<bool> abortUnzip(false);
std::atomic<int> downloadPercentage(-1);
std::atomic<int> unzipPercentage(-1);

// Thread-safe curl initialization
static std::mutex curlInitMutex;
static std::atomic<bool> curlInitialized(false);

const std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36";

// Definition of CurlDeleter
void CurlDeleter::operator()(CURL* curl) const {
    if (curl) {
        curl_easy_cleanup(curl);
    }
}

// Callback function to write received data to a file.
#if NO_FSTREAM_DIRECTIVE
// Using stdio.h functions (FILE*, fwrite)
size_t writeCallback(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!ptr || !stream) return 0;
    //size_t totalBytes = size * nmemb;
    //size_t writtenBytes = fwrite(ptr, 1, totalBytes, stream);
    //return writtenBytes;
    return fwrite(ptr, 1, size * nmemb, stream);
}
#else
// Using std::ofstream for writing
size_t writeCallback(void* ptr, size_t size, size_t nmemb, std::ostream* stream) {
    if (!ptr || !stream) return 0;
    auto& file = *static_cast<std::ofstream*>(stream);
    size_t totalBytes = size * nmemb;
    file.write(static_cast<const char*>(ptr), totalBytes);
    return totalBytes;
}
#endif

// Your C function
extern "C" int progressCallback(void *ptr, curl_off_t totalToDownload, curl_off_t nowDownloaded, curl_off_t totalToUpload, curl_off_t nowUploaded) {
    if (!ptr) return 1;
    
    auto percentage = static_cast<std::atomic<int>*>(ptr);

    if (totalToDownload > 0) {
        int newProgress = static_cast<int>((static_cast<double>(nowDownloaded) / static_cast<double>(totalToDownload)) * 100.0);
        percentage->store(newProgress, std::memory_order_release);
    }

    if (abortDownload.load(std::memory_order_acquire)) {
        percentage->store(-1, std::memory_order_release);
        return 1;  // Abort the download
    }

    return 0;  // Continue the download
}

// Global initialization function
void initializeCurl() {
    std::lock_guard<std::mutex> lock(curlInitMutex);
    if (!curlInitialized.load(std::memory_order_acquire)) {
        CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (res != CURLE_OK) {
            #if USING_LOGGING_DIRECTIVE
            logMessage("curl_global_init() failed: " + std::string(curl_easy_strerror(res)));
            #endif
            // Handle error appropriately, possibly exit the program
        } else {
            curlInitialized.store(true, std::memory_order_release);
        }
    }
}

// Global cleanup function
void cleanupCurl() {
    std::lock_guard<std::mutex> lock(curlInitMutex);
    if (curlInitialized.load(std::memory_order_acquire)) {
        curl_global_cleanup();
        curlInitialized.store(false, std::memory_order_release);
    }
}

/**
 * @brief Downloads a file from a URL to a specified destination.
 *
 * @param url The URL of the file to download.
 * @param toDestination The destination path where the file should be saved.
 * @return True if the download was successful, false otherwise.
 */
bool downloadFile(const std::string& url, const std::string& toDestination) {
    abortDownload.store(false, std::memory_order_release);

    if (url.find_first_of("{}") != std::string::npos) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Invalid URL: " + url);
        #endif
        return false;
    }

    std::string destination = toDestination;
    if (destination.back() == '/') {
        createDirectory(destination);
        size_t lastSlash = url.find_last_of('/');
        if (lastSlash != std::string::npos) {
            destination += url.substr(lastSlash + 1);
        } else {
            #if USING_LOGGING_DIRECTIVE
            logMessage("Invalid URL: " + url);
            #endif
            return false;
        }
    } else {
        createDirectory(destination.substr(0, destination.find_last_of('/')));
    }

    std::string tempFilePath = getParentDirFromPath(destination) + "." + getFileName(destination) + ".tmp";

#ifndef NO_FSTREAM_DIRECTIVE
    // Use ofstream if NO_FSTREAM_DIRECTIVE is not defined
    std::ofstream file(tempFilePath, std::ios::binary);
    if (!file.is_open()) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Error opening file: " + tempFilePath);
        #endif
        return false;
    }
#else
    // Alternative method of opening file (depending on your platform, like using POSIX open())
    FILE* file = fopen(tempFilePath.c_str(), "wb");
    if (!file) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Error opening file: " + tempFilePath);
        #endif
        return false;
    }

    // ADD THIS: Set up write buffer for better performance
    //std::unique_ptr<char[]> writeBuffer;
    //if (DOWNLOAD_WRITE_BUFFER > 0) {
    //    writeBuffer = std::make_unique<char[]>(DOWNLOAD_WRITE_BUFFER);
    //    // _IOFBF = full buffering, _IOLBF = line buffering, _IONBF = no buffering
    //    setvbuf(file, writeBuffer.get(), _IOFBF, DOWNLOAD_WRITE_BUFFER);
    //}
#endif

    // Ensure curl is initialized
    initializeCurl();

    std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
    if (!curl) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Error initializing curl.");
        #endif
#ifndef NO_FSTREAM_DIRECTIVE
        file.close();
#else
        fclose(file);
#endif
        return false;
    }

    downloadPercentage.store(0, std::memory_order_release);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
#ifndef NO_FSTREAM_DIRECTIVE
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &file);
#else
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file);
#endif

    // Small buffer for responsive abort
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &downloadPercentage);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS); // Enable HTTP/2
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2); // Force TLS 1.2

    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_BUFFERSIZE, DOWNLOAD_READ_BUFFER); // Increase buffer size

    // Add timeout options
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);   // 10 seconds to connect
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);   // 1 byte/s (virtually any progress)
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);  // 1 minutes of no progress

    CURLcode result = curl_easy_perform(curl.get());

#ifndef NO_FSTREAM_DIRECTIVE
    file.close();
#else
    fclose(file);
#endif

    if (result != CURLE_OK) {
        #if USING_LOGGING_DIRECTIVE
        if (result == CURLE_OPERATION_TIMEDOUT) {
            logMessage("Download timed out: " + url);
        } else if (result == CURLE_COULDNT_CONNECT) {
            logMessage("Could not connect to: " + url);
        } else {
            logMessage("Error downloading file: " + std::string(curl_easy_strerror(result)));
        }
        #endif
        deleteFileOrDirectory(tempFilePath);
        downloadPercentage.store(-1, std::memory_order_release);
        return false;
    }

#ifndef NO_FSTREAM_DIRECTIVE
    std::ifstream checkFile(tempFilePath);
    if (!checkFile || checkFile.peek() == std::ifstream::traits_type::eof()) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Error downloading file: Empty file");
        #endif
        deleteFileOrDirectory(tempFilePath);
        downloadPercentage.store(-1, std::memory_order_release);
        checkFile.close();
        return false;
    }
    checkFile.close();
#else
    // Alternative method for checking if the file is empty (POSIX example)
    struct stat fileStat;
    if (stat(tempFilePath.c_str(), &fileStat) != 0 || fileStat.st_size == 0) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Error downloading file: Empty file");
        #endif
        deleteFileOrDirectory(tempFilePath);
        downloadPercentage.store(-1, std::memory_order_release);
        return false;
    }
#endif

    downloadPercentage.store(100, std::memory_order_release);
    moveFile(tempFilePath, destination);
    return true;
}


/**
 * @brief Custom I/O function for opening files with larger buffer
 */
static voidpf ZCALLBACK fopen64_file_func_custom(voidpf opaque, const void* filename, int mode) {
    FILE* file = nullptr;
    const char* mode_fopen = nullptr;
    
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ)
        mode_fopen = "rb";
    else if (mode & ZLIB_FILEFUNC_MODE_EXISTING)
        mode_fopen = "r+b";
    else if (mode & ZLIB_FILEFUNC_MODE_CREATE)
        mode_fopen = "wb";

    if ((filename != nullptr) && (mode_fopen != nullptr)) {
        file = fopen((const char*)filename, mode_fopen);
        if (file && ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ)) {
            // Set 64KB buffer for reading the ZIP file - reduces syscalls
            static const size_t zipReadBufferSize = UNZIP_READ_BUFFER;
            setvbuf(file, nullptr, _IOFBF, zipReadBufferSize);
        }
    }
    return file;
}

/**
 * @brief Extracts files from a ZIP archive to a specified destination.
 *
 * Ultra-optimized single-pass extraction with smooth byte-based progress reporting
 * using miniz with proper 64-bit file support and streaming extraction.
 * Fixed memory leaks with RAII for proper resource cleanup on abort.
 * 
 * @param zipFilePath The path to the ZIP archive file.
 * @param toDestination The destination directory where files should be extracted.
 * @return True if the extraction was successful, false otherwise.
 */
bool unzipFile(const std::string& zipFilePath, const std::string& toDestination) {
    abortUnzip.store(false, std::memory_order_release);
    unzipPercentage.store(0, std::memory_order_release);

    // Time-based abort checking - pre-calculated constants
    u64 lastAbortCheck = armTicksToNs(armGetSystemTick());
    u64 currentNanos; // Reused for all tick operations
    bool success = true;

    // RAII wrapper for unzFile
    struct UnzFileManager {
        unzFile file = nullptr;
        
        UnzFileManager(const std::string& path) {
            zlib_filefunc64_def ffunc;
            fill_fopen64_filefunc(&ffunc);
            ffunc.zopen64_file = fopen64_file_func_custom;
            file = unzOpen2_64(path.c_str(), &ffunc);
        }
        
        ~UnzFileManager() {
            if (file) {
                unzClose(file);
                file = nullptr;
            }
        }
        
        bool is_valid() const { return file != nullptr; }
        operator unzFile() const { return file; }
    };

    // RAII wrapper for output file
    struct OutputFileManager {
        #if NO_FSTREAM_DIRECTIVE
        FILE* file = nullptr;
        std::unique_ptr<char[]> buffer;
        size_t bufferSize;
        
        OutputFileManager(size_t bufSize) : bufferSize(bufSize) {
            buffer = std::make_unique<char[]>(bufferSize);
        }
        
        bool open(const std::string& path) {
            close();
            file = fopen(path.c_str(), "wb");
            if (file) {
                setvbuf(file, buffer.get(), _IOFBF, bufferSize);
            }
            return file != nullptr;
        }
        
        void close() {
            if (file) {
                fclose(file);
                file = nullptr;
            }
        }
        
        bool is_open() const { return file != nullptr; }
        
        size_t write(const void* data, size_t size) {
            return file ? fwrite(data, 1, size, file) : 0;
        }
        
        ~OutputFileManager() { close(); }
        #else
        std::ofstream file;
        
        OutputFileManager(size_t bufSize) {
            // Constructor for consistency with FILE* version
        }
        
        bool open(const std::string& path) {
            close();
            file.open(path, std::ios::binary);
            if (file.is_open()) {
                file.rdbuf()->pubsetbuf(nullptr, UNZIP_WRITE_BUFFER);
            }
            return file.is_open();
        }
        
        void close() {
            if (file.is_open()) {
                file.close();
            }
        }
        
        bool is_open() const { return file.is_open(); }
        
        size_t write(const void* data, size_t size) {
            if (file.is_open()) {
                file.write(static_cast<const char*>(data), size);
                return file.good() ? size : 0;
            }
            return 0;
        }
        #endif
    };

    UnzFileManager zipFile(zipFilePath);
    if (!zipFile.is_valid()) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Failed to open zip file: " + zipFilePath);
        #endif
        return false;
    }

    // Get global info about the ZIP file
    unz_global_info64 globalInfo;
    if (unzGetGlobalInfo64(zipFile, &globalInfo) != UNZ_OK) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("Failed to get zip file info");
        #endif
        return false;
    }

    const uLong numFiles = globalInfo.number_entry;
    if (numFiles == 0) {
        #if USING_LOGGING_DIRECTIVE
        logMessage("No files found in archive");
        #endif
        return false;
    }

    // ALWAYS calculate total size for accurate byte-based progress
    ZPOS64_T totalUncompressedSize = 0;
    char tempFilenameBuffer[512];
    unz_file_info64 fileInfo;
    
    // First pass: calculate total uncompressed size
    int result = unzGoToFirstFile(zipFile);
    while (result == UNZ_OK) {
        // Time-based abort check at start of each file (only if 2+ seconds have passed)
        currentNanos = armTicksToNs(armGetSystemTick());
        if ((currentNanos - lastAbortCheck) >= 2000000000ULL) {
            if (abortUnzip.load(std::memory_order_relaxed)) {
                unzipPercentage.store(-1, std::memory_order_release);
                #if USING_LOGGING_DIRECTIVE
                logMessage("Extraction aborted during size calculation");
                #endif
                abortUnzip.store(false, std::memory_order_release);
                return false;
            }
            lastAbortCheck = currentNanos;
        }

        if (unzGetCurrentFileInfo64(zipFile, &fileInfo, tempFilenameBuffer, sizeof(tempFilenameBuffer), 
                                   nullptr, 0, nullptr, 0) == UNZ_OK) {
            const size_t nameLen = strlen(tempFilenameBuffer);
            if (nameLen > 0 && tempFilenameBuffer[nameLen - 1] != '/') {
                totalUncompressedSize += fileInfo.uncompressed_size;
            }
        }
        result = unzGoToNextFile(zipFile);
    }

    // Fallback to 1 if no actual data (avoid division by zero)
    if (totalUncompressedSize == 0) {
        totalUncompressedSize = 1;
    }

    #if USING_LOGGING_DIRECTIVE
    logMessage("Processing " + std::to_string(numFiles) + " files, " + 
              std::to_string(totalUncompressedSize) + " total bytes from archive");
    #endif

    // Pre-allocate ALL reusable strings and variables outside the main loop
    std::string fileName, extractedFilePath, directoryPath;
    fileName.reserve(512);
    extractedFilePath.reserve(1024);
    directoryPath.reserve(1024);
    
    // Single large buffer for extraction - reused for all files
    const size_t bufferSize = UNZIP_WRITE_BUFFER;
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(bufferSize);
    char filenameBuffer[512]; // Stack allocated for filename reading
    
    // Progress tracking variables - OPTIMIZED for smooth byte-based tracking
    ZPOS64_T totalBytesProcessed = 0;
    uLong filesProcessed = 0;
    int currentProgress = 0;  // Current percentage (0-100)
    
    // Calculate bytes per percentage point for smooth updates - FIXED CALCULATION
    const ZPOS64_T bytesPerPercent = (totalUncompressedSize + 99) / 100;  // Round up to avoid missing last percent
    ZPOS64_T nextPercentBoundary = bytesPerPercent;  // First boundary at 1%
    
    
    // Create output file manager
    OutputFileManager outputFile(bufferSize);
    
    // Loop variables moved outside
    bool extractSuccess;
    ZPOS64_T fileBytesProcessed;
    int bytesRead;
    
    // String operation variables
    const char* filename;
    size_t nameLen;
    size_t lastSlashPos;
    size_t invalid_pos;
    size_t start_pos;
    
    // Ensure destination directory exists
    createDirectory(toDestination);
    
    // Ensure destination ends with '/' - pre-allocate final string
    std::string destination;
    destination.reserve(toDestination.size() + 1);
    destination = toDestination;
    if (!destination.empty() && destination.back() != '/') {
        destination += '/';
    }
    
    // Extract files
    result = unzGoToFirstFile(zipFile);
    while (result == UNZ_OK && success) {
        // Time-based abort check at start of each file (only if 2+ seconds have passed)
        currentNanos = armTicksToNs(armGetSystemTick());
        if ((currentNanos - lastAbortCheck) >= 2000000000ULL) {
            if (abortUnzip.load(std::memory_order_relaxed)) {
                success = false;
                break; // RAII will handle cleanup
            }
            lastAbortCheck = currentNanos;
        }
        
        // Get current file info - reuse fileInfo variable
        if (unzGetCurrentFileInfo64(zipFile, &fileInfo, filenameBuffer, sizeof(filenameBuffer), 
                                   nullptr, 0, nullptr, 0) != UNZ_OK) {
            result = unzGoToNextFile(zipFile);
            continue;
        }

        filename = filenameBuffer;
        
        // Quick filename validation
        if (!filename || filename[0] == '\0') {
            result = unzGoToNextFile(zipFile);
            continue;
        }
        
        nameLen = strlen(filename);
        if (nameLen > 0 && filename[nameLen - 1] == '/') { // Skip directories
            result = unzGoToNextFile(zipFile);
            continue;
        }
        
        // Build extraction path - reuse allocated strings
        fileName.assign(filename, nameLen);
        extractedFilePath.clear();
        extractedFilePath = destination;
        extractedFilePath += fileName;
        
        // Optimized character cleaning - only if needed
        invalid_pos = extractedFilePath.find_first_of(":*?\"<>|");
        if (invalid_pos != std::string::npos) {
            start_pos = std::min(extractedFilePath.find(ROOT_PATH) + 5, extractedFilePath.size());
            auto it = extractedFilePath.begin() + start_pos;
            extractedFilePath.erase(std::remove_if(it, extractedFilePath.end(), [](char c) {
                return c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|';
            }), extractedFilePath.end());
        }

        // Open the current file in the ZIP
        if (unzOpenCurrentFile(zipFile) != UNZ_OK) {
            #if USING_LOGGING_DIRECTIVE
            logMessage("Could not open file in ZIP: " + fileName);
            #endif
            result = unzGoToNextFile(zipFile);
            continue;
        }

        // Create directory if needed
        lastSlashPos = extractedFilePath.find_last_of('/');
        if (lastSlashPos != std::string::npos) {
            directoryPath.assign(extractedFilePath, 0, lastSlashPos + 1);
            createDirectory(directoryPath);
        }

        // Open output file
        if (!outputFile.open(extractedFilePath)) {
            unzCloseCurrentFile(zipFile);
            #if USING_LOGGING_DIRECTIVE
            logMessage("Error creating file: " + extractedFilePath);
            #endif
            result = unzGoToNextFile(zipFile);
            continue;
        }

        // Extract file data in chunks
        extractSuccess = true;
        fileBytesProcessed = 0;
        
        while ((bytesRead = unzReadCurrentFile(zipFile, buffer.get(), bufferSize)) > 0) {
            // Time-based abort check - only every 2 seconds for optimal performance
            currentNanos = armTicksToNs(armGetSystemTick());
            if ((currentNanos - lastAbortCheck) >= 2000000000ULL) {
                if (abortUnzip.load(std::memory_order_relaxed)) {
                    extractSuccess = false;
                    break; // RAII will handle cleanup
                }
                lastAbortCheck = currentNanos;
            }
            
            // Write data to file
            if (outputFile.write(buffer.get(), bytesRead) != static_cast<size_t>(bytesRead)) {
                extractSuccess = false;
                break;
            }
            
            // Update progress tracking
            fileBytesProcessed += bytesRead;
            totalBytesProcessed += bytesRead;
            
            // FIXED: Check if we've crossed the next percentage boundary
            if (totalBytesProcessed >= nextPercentBoundary && currentProgress < 99) {
                currentProgress++;
                nextPercentBoundary = (static_cast<ZPOS64_T>(currentProgress + 1) * totalUncompressedSize) / 100;
                
                // Update the atomic progress value
                unzipPercentage.store(currentProgress, std::memory_order_relaxed);
                
                #if USING_LOGGING_DIRECTIVE
                // Only log at 10% intervals to avoid spam
                if (currentProgress % 10 == 0) {
                    logMessage("Progress: " + std::to_string(currentProgress) + "% (" + 
                              std::to_string(totalBytesProcessed) + "/" + 
                              std::to_string(totalUncompressedSize) + " bytes)");
                }
                #endif
            }
        }

        // Check for read errors
        if (bytesRead < 0) {
            extractSuccess = false;
        }

        // Close current file handles
        outputFile.close();
        unzCloseCurrentFile(zipFile);

        if (!extractSuccess) {
            deleteFileOrDirectory(extractedFilePath);
            #if USING_LOGGING_DIRECTIVE
            logMessage("Failed to extract: " + fileName);
            #endif
            
            if (abortUnzip.load(std::memory_order_relaxed)) {
                success = false;
                break;
            }
        } else {
            filesProcessed++;
        }

        // Move to next file
        result = unzGoToNextFile(zipFile);
    }

    // Check final abort state
    if (abortUnzip.load(std::memory_order_relaxed)) {
        unzipPercentage.store(-1, std::memory_order_release);
        #if USING_LOGGING_DIRECTIVE
        logMessage("Extraction aborted by user");
        #endif
        abortUnzip.store(false, std::memory_order_release);
        return false;
    }

    if (success && filesProcessed > 0) {
        abortUnzip.store(false, std::memory_order_release);
        unzipPercentage.store(100, std::memory_order_release);
        
        #if USING_LOGGING_DIRECTIVE
        logMessage("Extraction completed: " + std::to_string(filesProcessed) + " files, " + 
                  std::to_string(totalBytesProcessed) + " bytes");
        #endif
        
        return true;
    } else {
        abortUnzip.store(false, std::memory_order_release);
        unzipPercentage.store(-1, std::memory_order_release);
        return false;
    }
}

}
