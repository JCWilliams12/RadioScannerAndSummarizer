#ifndef DBCOREFUNCTIONS_HPP
#define DBCOREFUNCTIONS_HPP

#include <string>
#include <vector>

struct RadioLog {
    double freq;
    long long time;
    std::string location;
    std::string rawT;
    std::string summary;
    std::string channelName;
    std::string audioFilePath;
};

bool createTable();
void insertLog(double freq, long long time, std::string location, std::string rawT, std::string summary, std::string channelName);
int removeLog(double freq, long long time, std::string location);
void openDatabase();

std::vector<RadioLog> getAllLogs();

#endif