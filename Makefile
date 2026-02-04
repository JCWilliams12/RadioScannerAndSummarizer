# Variables
CC = gcc
CXX = g++
CFLAGS = -O2
# Added pthread and boost flags for Crow
CXXFLAGS = -std=c++17 -O2 -I./include
LIBS = -lpthread -lboost_system

# The final executable name (Linux standard is usually just 'main')
TARGET = main

# The object files
OBJS = main.o sqlite3.o

# Default target
all: $(TARGET)

# Linking the executable
# Added $(LIBS) here so Crow can use networking and threads
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LIBS)

# Compiling main.cpp as C++
main.o: main.cpp
	$(CXX) $(CXXFLAGS) -c main.cpp -o main.o

# Compiling sqlite3.c as C
sqlite3.o: sqlite3.c
	$(CC) $(CFLAGS) -c sqlite3.c -o sqlite3.o

# Clean up build files
clean:
	rm -f *.o $(TARGET)