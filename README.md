# RadioScannerAndSummarizer
# AuetherGuard

-=-=-=-=-=-=-TO RUN-=-=-=-=-=-=-=-=-=-
widnows:
./start.bat
or
./start.bat mock
!!! USE "./start.bat stop" TO CLOSE CLEANLY !!!


Mac/Linux:
./start
(can boot into mock mode automatically upon not detcting SDR unit)
!!! USE "./start.sh stop" TO CLOSE CLEANY !!!


for additional boot commands check out the start.bat and start.sh files
-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

Manual:
    Scan page:
    - find station button scans for all public radio stations in area
    - left table allows the user to select a station
    - listen live button allows user to listen to the station
        - if scan is active the user can only listen to the scanning station
    - scan button takes a 30 second sample of the selected radio station
        - this sample will be summarized by the AI selected in the top right
    - save button saves summary to the database
    - queue table allows user to queue up multiple scans
    - Favorite/star button allows user to continuously scan the same few stations, this is auto saved to DB

    DB page:
    - Left table displays all saved logs in chronological order
    - Playback button allows user to listen to the recorded sample
    - delete button allows user to delete entry
    - Filter dropdown allows the user to filter by multiple parameters
        - keywords: checks for keywords in the station name and summary/raw transcritpion
        - frequency
        - location
        - start/end date range
    - smart DB agent allows user to ask questions about content in the database
        - ex: "What stations broadcasted an emergency message on April 13th"





---------dev stuff might be important later----------
|||||||||||||||||||||||||||||||||||||||||||||||||||||
vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv

( ͡° ͜ʖ ͡°)
:^)

paste the following in the terminal:
Windows:
    cmake --build build
    .\build\Debug\server.exe
    
    cmake --build build --config Release
    .\build\Release\server.exe

Linux:
    brew install cmake
    from root:
        cmake -S . -B build
        cmake --build build
        ./build/server


-=-=-=-=-=-=-Run Front End-=-=-=-=-=-=-=-=-=-
Paste the following into terminal: 
cd client
npm install
npm run dev


docker-compose up --build
usbipd attach --wsl --busid <BUSID>
docker-compose down
docker-compose up --build


cd sdr-relay
cmake --build build
.\build\Debug\sdr_relay.exe


---- For mac first time: 
cmake -B build
cmake --build build
./build/sdr_relay

--- Then run: 
docker build -t aetherguard-base:latest -f Dockerfile.base .
docker-compose up --build



