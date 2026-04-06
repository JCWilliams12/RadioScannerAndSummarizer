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

// Helper function to populate a RadioLog struct from a sqlite3 statement
RadioLog populateLogFromStmt(sqlite3_stmt *stmt) {
    RadioLog log;
    log.freq          = sqlite3_column_double(stmt, 0);
    log.time          = sqlite3_column_int64(stmt, 1);
    log.location      = (const char*)sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
    log.rawT          = (const char*)sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
    log.summary       = (const char*)sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
    log.channelName   = (const char*)sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
    log.audioFilePath = (const char*)sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "";
    return log;
}

std::vector<RadioLog> filterByFrequency(double freq) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs WHERE freq BETWEEN ? AND ? ORDER BY time DESC;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_double(stmt, 1, freq - 0.01);
        sqlite3_bind_double(stmt, 2, freq + 0.01);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
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

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs WHERE location LIKE ? ORDER BY time DESC;";  

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        std::string query = "%" + loc + "%";
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
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

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs WHERE time >= ? AND time <= ? ORDER BY time ASC;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, unixTime);
        sqlite3_bind_int64(stmt, 2, unixTime + 59);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
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

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath "
                      "FROM RadioLogs WHERE channelName LIKE ? ORDER BY time DESC;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        std::string query = "%" + channel + "%";
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}

std::vector<RadioLog> filterByTimeRange(long long startTimestamp, long long endTimestamp) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs WHERE time >= ? AND time <= ? ORDER BY time DESC;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startTimestamp);
        sqlite3_bind_int64(stmt, 2, endTimestamp);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return results;
}

std::vector<RadioLog> searchDatabaseKeywords(const std::string& keyword, int limit) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    // Searches both the AI summary and the raw transcription
    const char *sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath "
                      "FROM RadioLogs WHERE summary LIKE ? OR rawT LIKE ? ORDER BY time DESC LIMIT ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        std::string query = "%" + keyword + "%";
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}

std::vector<RadioLog> advancedSearch(const std::string& freqStr, const std::string& loc, const std::string& keyword, const std::string& startStr, const std::string& endStr) {
    std::lock_guard<std::mutex> lock(db_mtx);
    sqlite3 *db;
    sqlite3_stmt *stmt;
    std::vector<RadioLog> results;

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) return results;

    // Base query
    std::string sql = "SELECT freq, time, location, rawT, summary, channelName, audioFilePath FROM RadioLogs WHERE 1=1";

    // Dynamically append AND clauses
    if (!freqStr.empty()) sql += " AND ABS(freq - ?) < 0.005";
    if (!loc.empty()) sql += " AND location LIKE ?";
    if (!keyword.empty()) sql += " AND (summary LIKE ? OR rawT LIKE ? OR channelName LIKE ?)";
    if (!startStr.empty() && !endStr.empty()) sql += " AND time BETWEEN ? AND ?";

    sql += " ORDER BY time DESC;";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0) == SQLITE_OK) {
        int bindIdx = 1;

        // Bind provided variables in exact order
        if (!freqStr.empty()) {
            sqlite3_bind_double(stmt, bindIdx++, std::stod(freqStr));
        }
        
        std::string locQuery;
        if (!loc.empty()) {
            locQuery = "%" + loc + "%";
            sqlite3_bind_text(stmt, bindIdx++, locQuery.c_str(), -1, SQLITE_TRANSIENT);
        }
        
        std::string keyQuery;
        if (!keyword.empty()) {
            keyQuery = "%" + keyword + "%";
            sqlite3_bind_text(stmt, bindIdx++, keyQuery.c_str(), -1, SQLITE_TRANSIENT); // summary
            sqlite3_bind_text(stmt, bindIdx++, keyQuery.c_str(), -1, SQLITE_TRANSIENT); // rawT
            sqlite3_bind_text(stmt, bindIdx++, keyQuery.c_str(), -1, SQLITE_TRANSIENT); // channelName
        }
        
        if (!startStr.empty() && !endStr.empty()) {
            sqlite3_bind_int64(stmt, bindIdx++, std::stoll(startStr));
            sqlite3_bind_int64(stmt, bindIdx++, std::stoll(endStr));
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(populateLogFromStmt(stmt));
        }
    } else {
        std::cerr << "Advanced Search SQL Error: " << sqlite3_errmsg(db) << std::endl;
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return results;
}