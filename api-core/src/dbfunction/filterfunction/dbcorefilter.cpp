#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <stdio.h>
#include <fstream>
#include "dbcorefilter.hpp"
#include "../corefunction/dbcorefunctions.hpp"

extern "C" {
    #include "sqlite3.h"
}

static std::mutex db_mtx;

std::vector<RadioLog> filterByFrequency(double freq) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName FROM RadioLogs WHERE freq BETWEEN ? AND ? ORDER BY time DESC;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, freq - 0.01);
        sqlite3_bind_double(stmt, 2, freq + 0.01);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioLog log;
            log.freq     = sqlite3_column_double(stmt, 0);
            log.time     = sqlite3_column_int64(stmt, 1);
            log.location = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            log.rawT     = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            log.summary  = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            log.channelName = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            results.push_back(log);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}

std::vector<RadioLog> filterByLocation(const std::string& loc) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName FROM RadioLogs WHERE location LIKE ? ORDER BY time DESC;";  

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        std::string query = "%" + loc + "%";
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioLog log;
            log.freq     = sqlite3_column_double(stmt, 0);
            log.time     = sqlite3_column_int64(stmt, 1);
            log.location = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            log.rawT     = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            log.summary  = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            log.channelName = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            results.push_back(log);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}

std::vector<RadioLog> filterByTime(long long unixTime) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName FROM RadioLogs WHERE time >= ? AND time <= ? ORDER BY time ASC;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, unixTime);
        sqlite3_bind_int64(stmt, 2, unixTime + 59);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioLog log;
            log.freq        = sqlite3_column_double(stmt, 0);
            log.time        = sqlite3_column_int64(stmt, 1);
            log.location    = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            log.rawT        = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            log.summary     = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            log.channelName = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            results.push_back(log);
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return results;
}

std::vector<RadioLog> filterByChannelName(const std::string& channel) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName "
                      "FROM RadioLogs WHERE channelName LIKE ? ORDER BY time DESC;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        std::string query = "%" + channel + "%";
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioLog log;
            log.freq        = sqlite3_column_double(stmt, 0);
            log.time        = sqlite3_column_int64(stmt, 1);
            log.location    = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
            log.rawT        = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
            log.summary     = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
            log.channelName = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
            results.push_back(log);
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}