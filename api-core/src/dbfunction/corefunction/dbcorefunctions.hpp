#ifndef DBCOREFUNCTIONS_HPP
#define DBCOREFUNCTIONS_HPP

#include <string>
#include <vector>

// The "Container" for your radio data
struct RadioLog {
    double freq;
    long long time;
    std::string location;
    std::string rawT;
    std::string summary;
    std::string channelName;
};

bool createTable();
void insertLog(double freq, long long time, std::string location, std::string rawT, std::string summary, std::string channelName);
int removeLog(double freq, long long time, std::string location);
void openDatabase();

// Updated to return a vector of the struct above
std::vector<RadioLog> getAllLogs();

#endif