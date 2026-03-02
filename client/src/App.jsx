import { useState, useEffect, useRef } from 'react'
import './App.css'

function App() {
  // State Management
  const [view, setView] = useState('home');
  const [stations, setStations] = useState([]);
  
  // SEPARATED states for live scanning
  const [activeSummary, setActiveSummary] = useState("Waiting for scan...");
  const [activeRawText, setActiveRawText] = useState(""); 
  
  // Selection states for each view
  const [selectedStation, setSelectedStation] = useState(null);
  const [selectedLog, setSelectedLog] = useState(null);
  const [logs, setLogs] = useState([]);

  // For Playback function on DB page
  const [showPlaybackMenu, setShowPlaybackMenu] = useState(false);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [volume, setVolume] = useState(50);
  
  // --- THE GLITCH FIX: Tracks if you are currently dragging the slider ---
  const [isDragging, setIsDragging] = useState(false);
  const [localAudioUrl, setLocalAudioUrl] = useState("");
  const audioRef = useRef(null); 

  // --- NEW: Helper function to format seconds into MM:SS ---
  const formatTime = (timeInSeconds) => {
    if (isNaN(timeInSeconds)) return "00:00";
    const m = Math.floor(timeInSeconds / 60).toString().padStart(2, '0');
    const s = Math.floor(timeInSeconds % 60).toString().padStart(2, '0');
    return `${m}:${s}`;
  };

  const fetchLogs = async () => {
    try {
      const res = await fetch('http://localhost:8080/api/logs'); 
      const data = await res.json();
      setLogs(data);
    } catch (err) {
      console.error("Failed to fetch logs:", err);
    }
  };

  const fetchStations = async () => {
    try {
      const res = await fetch('http://localhost:8080/stations');
      const data = await res.json();
      setStations(data);
    } catch (err) {
      console.error("Link to C++ failed:", err);
    }
  };

  useEffect(() => {
    fetchStations();
    fetchLogs(); 
  }, []);

// --- THE FIX: Fetch audio as a Blob to allow perfect scrubbing ---
useEffect(() => {
  let objectUrl = "";

  if (showPlaybackMenu && selectedLog) {
    fetch("http://localhost:8080/whispertinytest/audio.wav")
      .then(res => res.blob())
      .then(blob => {
        objectUrl = URL.createObjectURL(blob);
        setLocalAudioUrl(objectUrl); // Feed the local blob to the audio player
      })
      .catch(err => console.error("Error fetching audio:", err));
  }

  return () => {
    // Clean up the memory when the menu closes
    if (objectUrl) URL.revokeObjectURL(objectUrl);
  };
}, [showPlaybackMenu, selectedLog]);

  // --- THE NEW 2-STEP SCAN HANDLER ---
  const handleScan = async () => {
    if (!selectedStation) return;

    setActiveRawText("Transcribing audio from " + Number(selectedStation.freq).toFixed(3) + " MHz...");
    setActiveSummary("Waiting for transcription...");

    try {
      const transcribeRes = await fetch(`http://localhost:8080/api/transcribe`, {
        method: 'POST',
        body: JSON.stringify({ freq: parseFloat(selectedStation.freq) }),
      });

      if (!transcribeRes.ok) throw new Error("Transcription failed");
      const transcribeData = await transcribeRes.json();
      
      const rawText = transcribeData.transcription;
      setActiveRawText(rawText);
      setActiveSummary("Generating AI Summary...");

      const summaryRes = await fetch(`http://localhost:8080/api/summarize`, {
        method: 'POST',
        body: JSON.stringify({ text: rawText }),
      });

      if (!summaryRes.ok) throw new Error("Summarization failed");
      const summaryData = await summaryRes.json();
      
      setActiveSummary(summaryData.summary);

    } catch (error) {
      console.error("Scan error:", error);
      setActiveSummary("Error during scan process.");
      setActiveRawText("Scan failed. Check server console.");
    }
  };

  const handleSave = async () => {
    if (!selectedStation) {
      alert("Please select a frequency first!");
      return;
    }

    try {
      const response = await fetch(`http://localhost:8080/api/logs/save`, {
        method: 'POST',
        body: JSON.stringify({
          freq: parseFloat(selectedStation.freq),
          time: Math.floor(Date.now() / 1000), 
          location: "Birmingham, AL", 
          rawT: activeRawText,
          summary: activeSummary, 
          channelName: selectedStation.name
        }),
      });

      if (response.ok) {
        console.log("Successfully saved log for:", selectedStation.name);
        alert("Log saved successfully!");
        await fetchLogs();
      } else {
        alert("Failed to save log to the server.");
      }
    } catch (error) {
      console.error("Connection error:", error);
    }
  };

  const handleDelete = async () => {
    if (selectedLog && window.confirm(`Delete log for ${selectedLog.name}?`)) {
      try {
        const response = await fetch(`http://localhost:8080/api/logs/delete`, {
          method: 'POST', 
          body: JSON.stringify({
            freq: parseFloat(selectedLog.freq), 
            time: selectedLog.time,
            location: selectedLog.location 
          }),
        });
  
        if (response.ok) { 
          setSelectedLog(null);
          await fetchLogs(); 
        } else {
          alert("Failed to delete log.");
        }
      } catch (error) {
        console.error("Connection error:", error);
      }
    }
  };

  const resetView = () => {
    setSelectedStation(null);
    setSelectedLog(null);
    setActiveSummary("Waiting for scan...");
    setActiveRawText("");
    setView('home');
  };

  return (
    <div className="container">
      {/* HEADER SECTION */}
      <header className="app-header">
        <h1 className="logo-text">AetherGuard</h1>
        <p className="slogan">Radio Intelligence, Refined.</p>
        <div className="sponsor-tag">
          <span>Sponsored by </span>
          <span className="sponsor-name">Trideum</span>
        </div>
      </header>

      {/* HOME VIEW */}
      {view === 'home' && (
        <div className="card">
          <button className="main-btn" onClick={() => setView('scanning')}>Scan Now</button>
          <button className="main-btn" onClick={() => setView('database')}>Database</button>
        </div>
      )}
      
      {/* DATABASE VIEW */}
      {view === 'database' && (
        <div className="database-view-wrapper">
          <div className="scanning-grid">
            <div className="data-box">
              <h3>Saved Logs</h3>
              <ul className="frequency-list">
                {logs.map(log => (
                  <li 
                    key={log.id}
                    onClick={() => setSelectedLog(log)}
                    className={selectedLog?.id === log.id ? "active-station" : ""}
                  >
                    <div className="station-item-content">
                      <span className="freq-tag">{Number(log.freq).toFixed(3)} MHz</span>
                      <span className="station-name">{log.name || "Unknown"}</span>
                      <span className="station-time" style={{marginLeft: "10px", fontSize: "0.85em", color: "#aaa"}}>
                        {log.time ? new Date(log.time * 1000).toLocaleString(undefined, {
                          month: 'short', day: 'numeric', year: 'numeric', hour: '2-digit', minute: '2-digit'
                          }) : "Unknown"}
                      </span>
                    </div>
                  </li>
                ))}
              </ul>
            </div>

            <div className="data-box">
              <h3>Log Details</h3>
              <div className="summary-content">
                {selectedLog ? (
                  <>
                    <p className="summary-text"><strong>Station:</strong> {selectedLog.name}</p>
                    <p className="summary-text"><strong>Frequency:</strong> {Number(selectedLog.freq).toFixed(3)} MHz</p>
                    <p className="summary-text"><strong>Location:</strong> {selectedLog.location}</p>
                    
                    {/* NEW: Normal Time + Unix Time displayed together */}
                    <p className="summary-text">
                      <strong>Time:</strong> {selectedLog.time ? new Date(selectedLog.time * 1000).toLocaleString(undefined, {
                          month: 'short',
                          day: 'numeric',
                          year: 'numeric',
                          hour: '2-digit',
                          minute: '2-digit'
                      }) : "Unknown"} <span style={{ fontSize: "0.85em", color: "#aaa" }}>(Unix: {selectedLog.time})</span>
                    </p>
                    
                    <hr style={{ borderColor: '#333', margin: '10px 0' }} />
                    <p className="summary-text"><strong>AI Summary:</strong> {selectedLog.summary || "No summary available"}</p>
                    <br/>
                    <p className="summary-text" style={{ fontSize: "0.85em", color: "#bbb" }}>
                      <em>Raw Text: {selectedLog.rawT || "No raw text available"}</em>
                    </p>
                  </>
                ) : (
                  <p className="summary-text">Select a log to view details</p>
                )}
              </div>
              <div className="action-buttons" style={{ position: 'relative' }}>
                <button 
                  className="sub-btn scan-btn" 
                  onClick={() => setShowPlaybackMenu(!showPlaybackMenu)}
                  disabled={!selectedLog}
                >
                  Playback
                </button>
                <button 
                  className="sub-btn delete-btn" 
                  onClick={handleDelete} 
                  disabled={!selectedLog}
                >
                  Delete
                </button>

                {/* PLAYBACK POPUP MENU */}
                {showPlaybackMenu && selectedLog && (
                  <div className="playback-popup">
                    
                    {/* HIDDEN AUDIO ELEMENT */}
                    <audio 
                      ref={audioRef}
                      src={localAudioUrl} /* <-- CHANGED THIS */
                      onLoadedMetadata={(e) => setDuration(e.target.duration)}
                      onTimeUpdate={(e) => {
                        // FIX: Only update the slider if you are NOT dragging it
                        if (!isDragging) {
                          setCurrentTime(e.target.currentTime);
                        }
                      }}
                      onEnded={() => {
                        setIsPlaying(false);
                        setCurrentTime(0); // Reset to start when finished
                      }}
                    />
                    
                    {/* PLAY/PAUSE CONTROLS */}
                    <button 
                      onClick={() => {
                        if (!audioRef.current || !audioRef.current.src) return;
                        if (isPlaying) audioRef.current.pause();
                        else audioRef.current.play();
                        setIsPlaying(!isPlaying);
                      }}
                      style={{
                        padding: '8px',
                        backgroundColor: '#004080',
                        color: 'white',
                        border: 'none',
                        borderRadius: '4px',
                        cursor: 'pointer',
                        fontWeight: 'bold',
                        marginBottom: '5px'
                      }}
                    >
                      {isPlaying ? 'Pause' : 'Play'}
                    </button>

                    <div className="slider-group">
                      <label>Timeline: {formatTime(currentTime)} / {formatTime(duration)}</label>
                      <input 
                        type="range" 
                        min="0" 
                        max={duration || 100} 
                        step="0.01" 
                        value={currentTime} 
                        onMouseDown={() => setIsDragging(true)}   /* Stop fighting the mouse */
                        onTouchStart={() => setIsDragging(true)}  /* Stop fighting the touch screen */
                        onChange={(e) => {
                          setCurrentTime(Number(e.target.value)); /* Update visual instantly */
                        }}
                        onMouseUp={(e) => {
                          setIsDragging(false);
                          if (audioRef.current) {
                            audioRef.current.currentTime = Number(e.target.value); /* Actually move the audio */
                          }
                        }}
                        onTouchEnd={(e) => {
                          setIsDragging(false);
                          if (audioRef.current) {
                            audioRef.current.currentTime = Number(e.target.value);
                          }
                        }}
                      />
                    </div>
                    
                    <div className="slider-group">
                      <label>Volume: {volume}%</label>
                      <input 
                        type="range" 
                        min="0" 
                        max="100" 
                        step="1"
                        value={volume} 
                        onChange={(e) => {
                          const newVol = Number(e.target.value);
                          setVolume(newVol);
                          if (audioRef.current) {
                            audioRef.current.volume = newVol / 100;
                          }
                        }}
                      />
                    </div>
                  </div>
                )}
              </div>
            </div>
          </div>
          <div className="button-container">
            <button className="back-btn" onClick={resetView}>Back to Home</button>
          </div>
        </div>
      )}

      {/* SCANNING VIEW */}
      {view === 'scanning' && (
        <div className="scanning-container">
          <div className="scanning-grid">
            <div className="data-box">
              <h3>Live Frequencies</h3>
              <ul className="frequency-list">
                {stations.map(s => (
                  <li 
                    key={s.id}
                    onClick={() => setSelectedStation(s)}
                    className={selectedStation?.id === s.id ? "active-station" : ""}
                  >
                    <div className="station-item-content">
                      <span className="freq-tag">{Number(s.freq).toFixed(3)} MHz</span>
                      <span className="station-name">{s.name}</span>
                    </div>
                  </li>
                ))}
              </ul>
            </div>

            <div className="data-box">
              <h3>Transmission Summary</h3>
              <div className="summary-content">
                <p className="summary-text">
                  {selectedStation ? `Target: ${Number(selectedStation.freq).toFixed(3)} MHz` : "Select a frequency"}
                </p>
                <hr style={{ borderColor: '#333', margin: '10px 0' }} />
                
                {/* DISPLAY SUMMARY FIRST */}
                <p className="summary-text"><strong>AI Summary:</strong> {activeSummary}</p>
                
                {/* DISPLAY RAW TEXT SECOND */}
                {activeRawText && (
                  <>
                    <br/>
                    <p className="summary-text" style={{ fontSize: "0.85em", color: "#bbb" }}>
                      <em>Raw Text: {activeRawText}</em>
                    </p>
                  </>
                )}

              </div>
              <div className="action-buttons">
                <button className="sub-btn scan-btn" onClick={handleScan} disabled={!selectedStation}>Scan</button>
                <button className="sub-btn save-btn" onClick={handleSave} disabled={!selectedStation}>Save</button>
              </div>
            </div>
          </div>
          <div className="button-container">
            <button className="back-btn" onClick={resetView}>Back to Home</button>
          </div>
        </div>
      )}
    </div>
  )
}

export default App