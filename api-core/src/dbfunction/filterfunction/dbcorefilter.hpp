#ifndef DBCOREFILTERHPP
#define DBCOREFILTERHPP

#include <string>
#include <vector>
extern const char* DB_NAME;

#include "../corefunction/dbcorefunctions.hpp"

std::vector<RadioLog> filterByFrequency(double freq);
std::vector<RadioLog> filterByTime(long long unixTime);
std::vector<RadioLog> filterByLocation(const std::string& loc);
std::vector<RadioLog> filterByChannelName(const std::string& channel);
std::vector<RadioLog> filterByTimeRange(long long startTimestamp, long long endTimestamp);
std::vector<RadioLog> searchDatabaseKeywords(const std::string& keyword, int limit);

#endif