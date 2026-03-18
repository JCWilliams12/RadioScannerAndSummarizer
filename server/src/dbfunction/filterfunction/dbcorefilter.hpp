#ifndef DBCOREFILTERHPP
#define DBCOREFILTERHPP

#include <string>
#include <vector>
// Global database path
extern const char* DB_NAME;

#include "../corefunction/dbcorefunctions.hpp"

// Filter functions
std::vector<RadioLog> filterByFrequency(double freq);
std::vector<RadioLog> filterByTime(long long unixTime); // Updated name to match your .cpp
std::vector<RadioLog> filterByLocation(const std::string& loc);

#endif