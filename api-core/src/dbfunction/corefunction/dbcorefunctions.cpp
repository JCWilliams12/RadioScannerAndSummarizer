#include <iostream>
#include <string>
#include <stdio.h>
#include <vector>
#include "dbcorefunctions.hpp"

extern "C" {
    #include "sqlite3.h"
}

const char* DB_NAME = "/app/shared/db/app.db";

// Creates the table only if it doesn't exist yet.
bool createTable() {
    sqlite3 *db;
    char *zErrMsg = 0;
    int rc;

    rc = sqlite3_open(DB_NAME, &db);

    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return false;
    }

    // Schema: freq, time, location, rawT, summary, channelName, audioFilePath
    // Composite Primary Key: freq, time, location
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS RadioLogs (" \
        "freq REAL, " \
        "time INTEGER, " \
        "location TEXT, " \
        "rawT TEXT, " \
        "summary TEXT, " \
        "channelName TEXT, " \
        "audioFilePath TEXT, " \
        "PRIMARY KEY (freq, time, location));";

    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } else {
        fprintf(stdout, "Table 'RadioLogs' created or verified successfully with new schema.\n");
    }

    sqlite3_close(db);
    return true;
}

// Uses "INSERT OR REPLACE" to update existing logs that match the primary key
void insertLog(double freq, long long time, std::string location, std::string rawT, std::string summary, std::string channelName, std::string audioFilePath) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_open(DB_NAME, &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return;
    }

    const char *sql = "INSERT OR REPLACE INTO RadioLogs (freq, time, location, rawT, summary, channelName, audioFilePath) VALUES (?, ?, ?, ?, ?, ?, ?);";

    // Prepare the statement
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_double(stmt, 1, freq);
    sqlite3_bind_int64(stmt, 2, time);
    sqlite3_bind_text(stmt, 3, location.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, rawT.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, summary.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, channelName.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, audioFilePath.c_str(), -1, SQLITE_STATIC);

    // Execute
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Execution failed: %s\n", sqlite3_errmsg(db));
    } else {
        std::cout << "Log inserted successfully: " << channelName << " (" << freq << "MHz) at " << time << std::endl;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

std::vector<RadioLog> getAllLogs() {
    std::vector<RadioLog> logs;
    sqlite3 *db;
    sqlite3_stmt *stmt;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) {
        std::cerr << "Can't open database: "  << sqlite3_errmsg(db) << std::endl;
        return logs;
    }

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs ORDER BY time DESC;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) {
        std::cerr << "SQL error: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_close(db);
        return logs;
    }

    std::cout << "\n--- Terminal Debug: Fetching Logs ---" << std::endl;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RadioLog log;
        log.freq        = sqlite3_column_double(stmt, 0);
        log.time        = sqlite3_column_int64(stmt, 1);

        const unsigned char* loc = sqlite3_column_text(stmt, 2);
        log.location = loc ? reinterpret_cast<const char*>(loc) : "";

        const unsigned char* rawText = sqlite3_column_text(stmt, 3);
        log.rawT = rawText ? reinterpret_cast<const char*>(rawText) : "";

        const unsigned char* sum = sqlite3_column_text(stmt, 4);
        log.summary = sum ? reinterpret_cast<const char*>(sum) : "";

        const unsigned char* cName = sqlite3_column_text(stmt, 5);
        log.channelName = cName ? reinterpret_cast<const char*>(cName) : "";

        const unsigned char* aPath = sqlite3_column_text(stmt, 6);
        log.audioFilePath = aPath ? reinterpret_cast<const char*>(aPath) : "";

        std::cout << "Found: " << log.channelName << " | " << log.freq << " MHz" << std::endl;

        logs.push_back(log);
    }
    
    std::cout << "Total logs sent to frontend: " << logs.size() << "\n" << std::endl;

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return logs;
}

int removeLog(double freq, long long time, std::string location) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_open(DB_NAME, &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    const char *sql = "DELETE FROM RadioLogs WHERE ABS(freq - ?) < 0.005 AND time = ? AND location = ?;";
        
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }

    sqlite3_bind_double(stmt, 1, freq);
    sqlite3_bind_int64(stmt, 2, time);
    sqlite3_bind_text(stmt, 3, location.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db); 

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Delete failed: %s\n", sqlite3_errmsg(db));
    } else {
        if (changes > 0) {
            std::cout << "SUCCESS: Deleted log: " << freq << "MHz at " << time << std::endl;
        } else {
            std::cout << "FAILED: No exact match found to delete. (Freq: " << freq << ", Time: " << time << ")" << std::endl;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return changes > 0 ? 1 : 0; 
}

void openDatabase(){
    sqlite3 *db;
    char *zErrMsg = 0;
    int rc;

    rc = sqlite3_open(DB_NAME, &db);

    if( rc ) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
    } else {
        fprintf(stdout, "Opened database successfully\n");
    }
    sqlite3_close(db);
}