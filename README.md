# RadioScannerAndSummarizer

-=-=-=-=-=-=-TO RUN-=-=-=-=-=-=-=-=-=-
paste the following in the terminal:
Windows:
    cmake --build build
    .\build\Debug\server.exe
    
Linux:
    brew install cmake
    from root:
        cmake -S . -B build
        cmake --build build
        ./build/server

-=-=-=-=-=-=-For Crow.h Just in case-=-=-=-=-=-=-=-=-=-
wget https://github.com/CrowCpp/Crow/releases/download/v1.2.0/crow_all.h -O crow.h
sudo apt-get install -y libboost-all-dev



-=-=-=-=-=-=-Run Front End-=-=-=-=-=-=-=-=-=-
Paste the following into terminal: 
cd client
npm install
npm run dev
