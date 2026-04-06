import { useState, useEffect, useRef } from 'react'
import './App.css'
import AetherGuardLogo from './assets/AetherGuardLogo.png'
import { motion, AnimatePresence } from "framer-motion"

// Progress bar keyframes
const progressKeyframes = `
  @keyframes progressShimmer {
    0% { background-position: -200% 0; }
    100% { background-position: 200% 0; }
  }
  @keyframes statusPulse {
    0%, 100% { opacity: 0.6; }
    50% { opacity: 1; }
  }
`;
 
// Loading indicator component
const ScanProgress = ({ status, progress }) => {
  return (
    <div style={{
      display: 'flex',
      flexDirection: 'column',
      alignItems: 'center',
      justifyContent: 'center',
      gap: '16px',
      padding: '30px 20px',
      width: '100%',
    }}>
      <style>{progressKeyframes}</style>
 
      {/* Status text */}
      <p style={{
        color: '#8cb4d5',
        fontSize: '0.95em',
        fontWeight: '500',
        textAlign: 'center',
        animation: 'statusPulse 1.8s ease-in-out infinite',
        margin: 0,
      }}>
        {status}
      </p>
 
      {/* Progress bar track */}
      <div style={{
        width: '100%',
        maxWidth: '320px',
        height: '8px',
        backgroundColor: 'rgba(3, 3, 3, 0.2)',
        borderRadius: '4px',
        overflow: 'hidden',
        position: 'relative',
      }}>
        {/* Filled portion */}
        <div style={{
          width: `${progress}%`,
          height: '100%',
          borderRadius: '4px',
          background: 'linear-gradient(90deg, #002b5e, #0066cc, #002b5e)',
          backgroundSize: '200% 100%',
          animation: 'progressShimmer 2s linear infinite',
          transition: 'width 0.4s ease-out',
        }} />
      </div>
 
      {/* Percentage */}
      <span style={{
        color: '#5a8aad',
        fontSize: '0.8em',
        fontFamily: 'monospace',
        letterSpacing: '1px',
      }}>
        {progress}%
      </span>
    </div>
  );
};

// For logo animation
const HeaderLogo = ({ isHovered, onHoverStart, onHoverEnd }) => {
  // Wi-Fi arc animation variants
  const arcVariants = {
    initial: { pathLength: 0, opacity: 0 },
    active: (i) => ({
      pathLength: [0, 1, 1, 0],
      opacity: [0, 0.9, 0.9, 0],
      transition: {
        duration: 2,
        ease: "easeInOut",
        repeat: Infinity,
        delay: i * 0.25,
        times: [0, 0.3, 0.7, 1],
      },
    }),
  };
 
  // Three arcs — smallest to largest
  const arcs = [
    { r: 14, strokeWidth: 5 },
    { r: 22, strokeWidth: 4.5 },
    { r: 30, strokeWidth: 4 },
  ];
 
  return (
    <motion.div
      className="logo-container"
      onMouseEnter = {onHoverStart}
      onMouseLeave={onHoverEnd}
      style={{
        position: 'relative',
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        cursor: 'pointer',
      }}
    >
      {/* Wi-Fi arcs SVG — top-right corner of the logo */}
      <svg
        width="80"
        height="80"
        viewBox="0 0 80 80"
        fill="none"
        style={{
          position: 'absolute',
          top: '76px',
          right: '-45px',
          rotate: '294deg',
          zIndex: 0,
          overflow: 'visible',
          pointerEvents: 'none',
        }}
      >
        {arcs.map((arc, i) => (
          <motion.path
            key={i}
            custom={i}
            initial="initial"
            animate={isHovered ? "active" : "initial"}
            variants={arcVariants}
            /* Quarter-circle arc: starts at (r, 0) and sweeps to (0, r) */
            d={`M ${arc.r} 0 A ${arc.r} ${arc.r} 0 0 1 0 ${arc.r}`}
            transform="translate(4, 4)"
            stroke="#002b5e"
            strokeWidth={arc.strokeWidth}
            strokeLinecap="round"
            fill="none"
          />
        ))}
      </svg>
 
      <img
        src={AetherGuardLogo}
        alt="AetherGuard Logo"
        className="logo"
        style={{ position: 'relative', zIndex: 1 }}
      />
    </motion.div>
  );
};

function App() {
  // --- Shared hover state for Wi-Fi arc animation ---
  const [logoHovered, setLogoHovered] = useState(false);

  // State Management
  const [view, setView] = useState('home');
  const [stations, setStations] = useState([]);
  
  // NEW: Dictionary to track scan jobs by frequency (solves the dropped call bug)
  const [scanJobs, setScanJobs] = useState({});

  // Helper function to safely update a specific frequency's background job
  const updateJob = (freq, updates) => {
    setScanJobs(prevJobs => ({
      ...prevJobs,
      [freq]: { ...(prevJobs[freq] || {}), ...updates }
    }));
  };
  
  // Selection states for each view
  const [selectedStation, setSelectedStation] = useState(null);
  const [selectedLog, setSelectedLog] = useState(null);
  const [logs, setLogs] = useState([]);

  // --- Advanced Filtering State ---
  const [showFilters, setShowFilters] = useState(false);
  const [isSearchActive, setIsSearchActive] = useState(false);
  const [filterFreq, setFilterFreq] = useState("");
  const [filterLoc, setFilterLoc] = useState("");
  const [filterKeyword, setFilterKeyword] = useState("");
  const [filterStart, setFilterStart] = useState("");
  const [filterEnd, setFilterEnd] = useState("");

  // --- Scan loading state ---
  const [isScanning, setIsScanning] = useState(false);
  const [scanStatus, setScanStatus] = useState("");
  const [scanProgress, setScanProgress] = useState(0);


  // For Playback function on DB page
  const [showPlaybackMenu, setShowPlaybackMenu] = useState(false);
  const [currentTime, setCurrentTime] = useState(0);
  const [duration, setDuration] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [volume, setVolume] = useState(50);
  
  const [isDragging, setIsDragging] = useState(false);
  const [localAudioUrl, setLocalAudioUrl] = useState("");
  const audioRef = useRef(null); 
  const playbackMenuRef = useRef(null);


  // WebSockets & Live Audio State
  const [isListeningLive, setIsListeningLive] = useState(false);
  const audioCtxRef = useRef(null);
  const wsRef = useRef(null);

  // Without this, all chunks fire at "now" causing rhythmic gaps/stuttering
  const nextPlayTimeRef = useRef(0);

  // Scanner State
  const [isScanningBand, setIsScanningBand] = useState(false);

  const formatTime = (timeInSeconds) => {
    if (isNaN(timeInSeconds)) return "00:00";
    const m = Math.floor(timeInSeconds / 60).toString().padStart(2, '0');
    const s = Math.floor(timeInSeconds % 60).toString().padStart(2, '0');
    return `${m}:${s}`;
  };

  const [useOpenAI, setUseOpenAI] = useState(false);

  const fetchLogs = async () => {
    try {
      const res = await fetch('/api/logs'); 
      const data = await res.json();
      setLogs(data);
      setIsSearchActive(false);
      clearFilterInputs();
    } catch (err) {
      console.error("Failed to fetch logs:", err);
    }
  };

  const clearFilterInputs = () => {
    setFilterFreq(""); setFilterLoc(""); setFilterKeyword(""); setFilterStart(""); setFilterEnd("");
  };

  const fetchStations = async () => {
    try {
      const res = await fetch('/stations');
      const data = await res.json();
      setStations(data);
    } catch (err) {
      console.error("Link to C++ failed:", err);
    }
  };

  const executeAdvancedSearch = async () => {
    try {
      const params = new URLSearchParams();
      if (filterFreq) params.append('freq', filterFreq);
      if (filterLoc) params.append('loc', filterLoc);
      if (filterKeyword) params.append('keyword', filterKeyword);
      
      // Convert browser Local Time to Unix Epochs for the C++ backend
      if (filterStart && filterEnd) {
        const startUnix = Math.floor(new Date(filterStart).getTime() / 1000);
        const endUnix = Math.floor(new Date(filterEnd).getTime() / 1000);
        params.append('start', startUnix);
        params.append('end', endUnix);
      } else if (filterStart || filterEnd) {
        alert("Please provide both a Start and End time for a date range search.");
        return;
      }

      const response = await fetch(`http://localhost:8080/api/search/advanced?${params.toString()}`);
      const data = await response.json();
      setLogs(data); 
      setIsSearchActive(true);
    } catch (error) {
      console.error("Failed to fetch advanced search results:", error);
    }
  };



  useEffect(() => {
    fetchStations();
    fetchLogs(); 
  }, []);


  // Close Playback menu when clicking outside of it
  useEffect(() => {
    const handleClickOutside = (event) => {
      if (
        playbackMenuRef.current && 
        !playbackMenuRef.current.contains(event.target) &&
        event.target.id !== 'playback-toggle-btn'
      ) {
        setShowPlaybackMenu(false);
      }
    };

    if (showPlaybackMenu) {
      document.addEventListener('mousedown', handleClickOutside);
    }
    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
    };
  }, [showPlaybackMenu]);

  
  // WIDEBAND FM SPECTRUM SWEEP
  const handleWidebandSweep = async () => {
    setIsScanningBand(true);
    console.log("Hardware: Performing 88-108 MHz sweep...");
    try {
      const res = await fetch('/api/scan/wideband');
      const data = await res.json();
      
      setStations(data.stations.map((s, index) => ({
        id: `found-${index}`,
        name: s.name,
        freq: s.freq.toString()
      })));
      
      console.log(`Sweep complete. Found ${data.stations.length} stations.`);
    } catch (err) {
      console.error("Sweep failed:", err);
      alert("Hardware error during spectrum sweep.");
    } finally {
      setIsScanningBand(false);
    }
  };

// LIVE AUDIO WEBSOCKET & HARDWARE TUNING HOOK
  useEffect(() => {
    if (isListeningLive && selectedStation) {
      audioCtxRef.current = new (window.AudioContext || window.webkitAudioContext)({
        sampleRate: 16000
      });

      // Reset the scheduler so the first chunk plays immediately
      nextPlayTimeRef.current = 0;
      
      wsRef.current = new WebSocket(`ws://${window.location.host}/ws/audio`);
      wsRef.current.binaryType = 'arraybuffer';

      wsRef.current.onmessage = (event) => {
        if (!audioCtxRef.current) return;
        
        const pcm16 = new Int16Array(event.data);
        // Buffer set to exactly 16000 Hz to prevent playback distortion
        const audioBuffer = audioCtxRef.current.createBuffer(1, pcm16.length, 16000);
        const channelData = audioBuffer.getChannelData(0);
        for (let i = 0; i < pcm16.length; i++) {
          channelData[i] = pcm16[i] / 32768.0; 
        }

        const source = audioCtxRef.current.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(audioCtxRef.current.destination);

        // Schedule chunks back-to-back using a running clock
        const currentTime = audioCtxRef.current.currentTime;
        if (nextPlayTimeRef.current < currentTime) {
          // Fell behind (underrun) - re-sync with a small 50ms buffer
          nextPlayTimeRef.current = currentTime + 0.05;
        }
        source.start(nextPlayTimeRef.current);
        nextPlayTimeRef.current += audioBuffer.duration;
      };

      fetch('/api/scan/live', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'start', freq: parseFloat(selectedStation.freq) })
      }).catch(err => console.error("Hardware live start error:", err));
      
    } else {
      // Clean up connections
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
      }
      if (audioCtxRef.current) {
        audioCtxRef.current.close();
        audioCtxRef.current = null;
      }
      
      fetch('/api/scan/live', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'stop' })
      }).catch(err => console.error("Hardware live stop error:", err));
    }

    return () => {
      // Ensure strict cleanup on component unmount
      if (wsRef.current) wsRef.current.close();
      if (audioCtxRef.current) audioCtxRef.current.close();
      if (isListeningLive) {
        fetch('/api/scan/live', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: 'stop' })
        }).catch(err => console.error("Hardware live stop error:", err));
      }
    };
  }, [isListeningLive, selectedStation]);

  // Fetch audio as a Blob to allow perfect scrubbing
  useEffect(() => {
    let objectUrl = "";

    if (showPlaybackMenu && selectedLog) {
      // Pull dynamic audio path if available, fallback to default
      const fetchPath = selectedLog.audioFilePath || "/api/audio/audio.wav";
      fetch(fetchPath)
        .then(res => res.blob())
        .then(blob => {
          objectUrl = URL.createObjectURL(blob);
          setLocalAudioUrl(objectUrl);
        })
        .catch(err => console.error("Error fetching audio:", err));
    }

    return () => {
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [showPlaybackMenu, selectedLog]);

  // THE 3-STEP SCAN HANDLER (Record -> Transcribe -> Summarize)
// THE ASYNC PIPELINE: WebSocket listener drives transcription + summarization.
  // handleScan only kicks off the recording. This listener handles the rest.
  useEffect(() => {
    const statusWs = new WebSocket(`ws://${window.location.host}/ws/status`);
    
    statusWs.onmessage = (event) => {
      const data = JSON.parse(event.data);
      const freq = data.freq;

      if (data.event === "record_complete") {
        // Recording finished → kick off transcription
        setScanProgress(50);
        setScanStatus("AI: Transcribing audio...");
        updateJob(freq, { summary: "AI: Processing transcription..." });
        
        fetch(useOpenAI ? '/api/transcribe/openai' : '/api/transcribe/local', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ freq: parseFloat(freq) }),
        });
      } 
      else if (data.event === "transcription_complete") {
        // Transcription finished → show text immediately, kick off summary
        setScanProgress(75);
        setScanStatus("AI: Generating summary...");
        updateJob(freq, { 
          rawText: data.text,
          summary: `Generating AI Summary via ${useOpenAI ? 'ChatGPT' : 'Local'}...`
        });

        fetch(useOpenAI ? '/api/summarize/openai' : '/api/summarize/local', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ text: data.text, freq: parseFloat(freq) }),
        });
      }
      else if (data.event === "summary_complete") {
        // Summary finished → show it, then dismiss the loading bar
        setScanProgress(100);
        setScanStatus("Scan complete!");
        updateJob(freq, { 
          status: "complete",
          summary: data.summary 
        });

        // Brief pause so user sees "100% / Scan complete!", then dismiss
        setTimeout(() => {
          setIsScanning(false);
          setScanProgress(0);
        }, 1200);
      }
    };

    return () => statusWs.close();
  }, [useOpenAI]);


  // SCAN HANDLER: kicks off recording only. The WebSocket listener above
  // drives transcription → summarization → completion automatically.
  const handleScan = async () => {
    if (!selectedStation) return;
    const targetFreq = selectedStation.freq;

    setIsScanning(true);
    setScanProgress(5);
    setScanStatus(`Connecting to ${Number(targetFreq).toFixed(3)} MHz...`);

    updateJob(targetFreq, { 
      status: "recording",
      summary: "Hardware: Recording 30-second capture...",
      rawText: `Capturing audio from ${Number(targetFreq).toFixed(3)} MHz...`
    });

    try {
      setScanProgress(10);
      setScanStatus("Hardware: Starting 30-second recording...");
      
      const recordRes = await fetch('/api/scan/record', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ freq: parseFloat(targetFreq) })
      });

      let recordData = null;
      try {
        recordData = await recordRes.json();
      } catch {
        throw new Error(`Record endpoint returned non-JSON (status: ${recordRes.status}).`);
      }

      if (!recordRes.ok || recordData?.status !== "recording_started") {
        throw new Error(`Recording failed. Status: ${recordRes.status}`);
      }

      // Animate progress bar during the 30-second recording
      const recordingStart = Date.now();
      const recordingDuration = 31000;
      const progressInterval = setInterval(() => {
        const elapsed = Date.now() - recordingStart;
        const ratio = Math.min(elapsed / recordingDuration, 1);
        const currentProgress = Math.round(10 + ratio * 35); // 10% → 45%
        setScanProgress(currentProgress);

        const secondsLeft = Math.max(0, Math.ceil((recordingDuration - elapsed) / 1000));
        setScanStatus(`Hardware: Recording in progress... (${secondsLeft}s remaining)`);

        if (ratio >= 1) clearInterval(progressInterval);
      }, 500);

      await new Promise(resolve => setTimeout(resolve, recordingDuration));
      clearInterval(progressInterval);

      // Recording timer done — WebSocket listener takes over from here.
      // Keep the loading bar visible while waiting for record_complete event.
      setScanProgress(48);
      setScanStatus("Waiting for AI pipeline...");

    } catch (error) {
      console.error(`Scan error on ${targetFreq}:`, error);
      updateJob(targetFreq, { 
        status: "error",
        summary: `Error: ${error.message}`,
        rawText: "Scan failed. Check server console."
      });
      setScanStatus(`Error: ${error.message}`);
      setScanProgress(0);
      await new Promise(r => setTimeout(r, 2000));
      setIsScanning(false);
    }
  };

  const handleSave = async () => {
    if (!selectedStation) {
      alert("Please select a frequency first!");
      return;
    }

    const targetFreq = selectedStation.freq;
    const jobData = scanJobs[targetFreq];

    if (!jobData || jobData.status !== "complete") {
      alert("Please wait for the scan to finish before saving!");
      return;
    }

    try {
      const response = await fetch('/api/logs/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          freq: parseFloat(targetFreq),
          time: Math.floor(Date.now() / 1000), 
          location: "Birmingham, AL", 
          rawT: jobData.rawText,       
          summary: jobData.summary,    
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
        const response = await fetch('/api/logs/delete', {
          method: 'POST', 
          headers: { 'Content-Type': 'application/json' },
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
    setIsListeningLive(false);
    setView('home');
  };

  // UI DERIVATIONS (Extracting the current job status for the UI dynamically)
  const currentJob = selectedStation ? scanJobs[selectedStation.freq] : null;
  const displaySummary = currentJob ? currentJob.summary : (isScanningBand ? "Hardware: Performing 88-108 MHz sweep..." : "Waiting for scan...");
  const displayRawText = currentJob ? currentJob.rawText : "";

  return (
    <div className="container">
      
      {/* Toggle Button anchored to the browser window */}
      <button 
        onClick={() => setUseOpenAI(!useOpenAI)}
        style={{
          position: 'fixed',
          top: '20px',
          right: '20px',
          zIndex: 1000,
          padding: '8px 16px',
          backgroundColor: useOpenAI ? '#10a37f' : '#3b82f6',
          color: 'white',
          border: 'none',
          borderRadius: '20px',
          cursor: 'pointer',
          fontWeight: 'bold',
          transition: 'background-color 0.3s',
          boxShadow: '0 4px 6px rgba(0,0,0,0.1)'
        }}
      >
        Mode: {useOpenAI ? "ChatGPT (Cloud)" : "Ollama (Local)"}
      </button>

      {/* HEADER SECTION */}
      <header className="app-header">
        {view === 'home' && (
          <HeaderLogo 
            isHovered={logoHovered}
            onHoverStart={() => setLogoHovered(true)}
            onHoverEnd={() => setLogoHovered(false)}
          />
        )}
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
          <button className="main-btn" 
            onClick={() => setView('scanning')}
            onMouseEnter={() => setLogoHovered(true)}
            onMouseLeave ={() => setLogoHovered(false)} 
            >Scan Now
          </button>
          <button className="main-btn" onClick={() => setView('database')}>Database</button>
        </div>
      )}
      
      {/* DATABASE VIEW */}
      {view === 'database' && (
        <div className="database-view-wrapper">
          <div className="scanning-grid">
            <div className="data-box">
              {/* ADVANCED FILTER HEADER */}
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '10px' }}>
                <h3 style={{ margin: 0, borderBottom: 'none' }}>Saved Logs</h3>
                <button 
                  onClick={() => setShowFilters(!showFilters)}
                  style={{ backgroundColor: 'transparent', color: '#8cb4d5', border: '1px solid #004080', padding: '4px 10px', borderRadius: '4px', cursor: 'pointer', fontSize: '0.85em' }}
                >
                  {showFilters ? 'Hide Filters ▲' : 'Advanced Filters ▼'}
                </button>
              </div>

              {/* ADVANCED FILTER PANEL */}
              {showFilters && (
                <div style={{ backgroundColor: 'rgba(0,0,0,0.2)', padding: '10px', borderRadius: '6px', marginBottom: '15px', display: 'flex', flexDirection: 'column', gap: '8px', border: '1px solid #002b5e' }}>
                  <input type="text" placeholder="Keyword (Search AI & Transcripts)" className="search-input" value={filterKeyword} onChange={(e) => setFilterKeyword(e.target.value)} />
                  
                  <div style={{ display: 'flex', gap: '8px' }}>
                    <input type="number" placeholder="Freq (MHz)" className="search-input" style={{ flex: 1 }} value={filterFreq} onChange={(e) => setFilterFreq(e.target.value)} />
                    <input type="text" placeholder="Location" className="search-input" style={{ flex: 1 }} value={filterLoc} onChange={(e) => setFilterLoc(e.target.value)} />
                  </div>
                  
                  <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
                    <div style={{ display: 'flex', flexDirection: 'column', flex: 1 }}>
                      <label style={{ fontSize: '0.75em', color: '#aaa', marginBottom: '2px' }}>Start Time</label>
                      <input type="datetime-local" className="search-input" value={filterStart} onChange={(e) => setFilterStart(e.target.value)} />
                    </div>
                    <div style={{ display: 'flex', flexDirection: 'column', flex: 1 }}>
                      <label style={{ fontSize: '0.75em', color: '#aaa', marginBottom: '2px' }}>End Time</label>
                      <input type="datetime-local" className="search-input" value={filterEnd} onChange={(e) => setFilterEnd(e.target.value)} />
                    </div>
                  </div>
                  
                  <div style={{ display: 'flex', justifyContent: 'flex-end', gap: '10px', marginTop: '5px' }}>
                    {isSearchActive && (
                      <button onClick={fetchLogs} style={{ backgroundColor: '#800000', color: 'white', border: 'none', padding: '6px 12px', borderRadius: '4px', cursor: 'pointer' }}>Clear</button>
                    )}
                    <button onClick={executeAdvancedSearch} style={{ backgroundColor: '#004080', color: 'white', border: 'none', padding: '6px 16px', borderRadius: '4px', cursor: 'pointer' }}>Search DB</button>
                  </div>
                </div>
              )}
              <ul className="frequency-list">
                {logs.map(log => (
                  <li 
                    key={`${log.freq}-${log.time}`}
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
              <div className="summary-content" style={{ height: '400px', overflowY: 'auto', paddingRight: '15px' }}>
                {selectedLog ? (
                  <>
                    <p className="summary-text"><strong>Station:</strong> {selectedLog.name}</p>
                    <p className="summary-text"><strong>Frequency:</strong> {Number(selectedLog.freq).toFixed(3)} MHz</p>
                    <p className="summary-text"><strong>Location:</strong> {selectedLog.location}</p>
                    <p className="summary-text">
                      <strong>Time:</strong> {selectedLog.time ? new Date(selectedLog.time * 1000).toLocaleString(undefined, {
                          month: 'short', day: 'numeric', year: 'numeric', hour: '2-digit', minute: '2-digit'
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
                  id="playback-toggle-btn" 
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
                  <div className="playback-popup" ref={playbackMenuRef}>
                    <audio 
                      ref={audioRef}
                      src={localAudioUrl}
                      onLoadedMetadata={(e) => setDuration(e.target.duration)}
                      onTimeUpdate={(e) => {
                        if (!isDragging) setCurrentTime(e.target.currentTime);
                      }}
                      onEnded={() => {
                        setIsPlaying(false);
                        setCurrentTime(0);
                      }}
                    />
                    
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
                        onMouseDown={() => setIsDragging(true)}
                        onTouchStart={() => setIsDragging(true)}
                        onChange={(e) => setCurrentTime(Number(e.target.value))}
                        onMouseUp={(e) => {
                          setIsDragging(false);
                          if (audioRef.current) audioRef.current.currentTime = Number(e.target.value);
                        }}
                        onTouchEnd={(e) => {
                          setIsDragging(false);
                          if (audioRef.current) audioRef.current.currentTime = Number(e.target.value);
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
                          if (audioRef.current) audioRef.current.volume = newVol / 100;
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
              <h3 style={{ marginBottom: '15px' }}>Live Frequencies</h3>
              
              <button 
                className="sub-btn" 
                onClick={handleWidebandSweep}
                disabled={isScanningBand || isScanning}
                style={{ 
                  width: '100%',      
                  marginBottom: '15px', 
                  padding: '12px',
                  backgroundColor: isScanningBand ? '#f39c12' : '#3b82f6',
                  color: 'white',
                  fontWeight: 'bold',
                  display: 'flex',
                  justifyContent: 'center',
                  alignItems: 'center',
                  border: 'none',
                  borderRadius: '6px',
                  cursor: isScanningBand ? 'wait' : 'pointer',
                  transition: 'background-color 0.3s ease'
                }}
              >
                {isScanningBand ? "Sweeping Spectrum (88-108 MHz)..." : "Auto-Find Stations"}
              </button>

              {/* Zeroed out default browser list spacing and made it scrollable */}
              <ul className="frequency-list" style={{ height: '400px', overflowY: 'auto', marginTop: 0, paddingLeft: 0, paddingRight: '5px', listStyle: 'none', opacity: isScanning ? 0.5 : 1, pointerEvents: isScanning ? 'none' : 'auto' }}>
                {stations.map(s => (
                  <li 
                    key={s.id}
                    onClick={() => {
                      if (isScanning) return;
                      setSelectedStation(s);
                      setIsListeningLive(false);
                    }}
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
                {/* Progress bar shows on top during scanning, results always visible below */}
                {isScanning && <ScanProgress status={scanStatus} progress={scanProgress} />}
                
                <p className="summary-text">
                  {selectedStation ? `Target: ${Number(selectedStation.freq).toFixed(3)} MHz` : "Select a frequency"}
                </p>
                <hr style={{ borderColor: '#333', margin: '10px 0' }} />
                
                <p className="summary-text"><strong>AI Summary:</strong> {displaySummary}</p>

                {displayRawText && (
                  <>
                    <br/>
                    <p className="summary-text" style={{ fontSize: "0.85em", color: "#bbb" }}>
                      <em>Raw Text: {displayRawText}</em>
                    </p>
                  </>
                )}
              </div>
                <div className="action-buttons">
                {/* RESTORED: Listen Live Toggle */}
                <button 
                  className="sub-btn" 
                  style={{ 
                    backgroundColor: isListeningLive ? '#e74c3c' : '#f39c12',
                    color: 'white' 
                  }}
                  onClick={() => setIsListeningLive(!isListeningLive)} 
                  disabled={!selectedStation}
                >
                  {isListeningLive ? "Stop Live Audio" : "Listen Live"}
                </button>

                <button 
                  className="sub-btn scan-btn" 
                  onClick={handleScan} 
                  disabled={!selectedStation || isScanning}
                >
                  {isScanning ? "Scanning..." : "Scan"}
                </button>
                
                <button 
                  className="sub-btn save-btn" 
                  onClick={handleSave} 
                  disabled={!selectedStation || isScanning || currentJob?.status !== 'complete'}
                >
                  Save
                </button>
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