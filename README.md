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
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> d1503cad464ff592a7ee36479702b59cfe15fe71


docker-compose up --build
usbipd attach --wsl --busid <BUSID>
<<<<<<< HEAD
=======
git 
>>>>>>> 1e5ab748bb8441ba63521c633fa58c18256b0cbf
=======
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



>>>>>>> d1503cad464ff592a7ee36479702b59cfe15fe71
