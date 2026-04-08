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
            animate={"active"}
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

  // State for search term
  const [searchTerm, setSearchTerm] = useState("");

  // State that tracks whether a search is active.
  const [isSearchActive, setIsSearchActive] = useState(false);

  // --- Advanced Filtering State ---
  const [showFilters, setShowFilters] = useState(false);
  const [filterFreq, setFilterFreq] = useState("");
  const [filterLoc, setFilterLoc] = useState("");
  const [filterKeyword, setFilterKeyword] = useState("");
  const [filterStart, setFilterStart] = useState("");
  const [filterEnd, setFilterEnd] = useState("");

  // --- AI Agent State ---
  const [isAgentOpen, setIsAgentOpen] = useState(false);
  const [agentMessages, setAgentMessages] = useState([]);
  const [agentInput, setAgentInput] = useState("");
  const [isAgentThinking, setIsAgentThinking] = useState(false);

  // --- Scan loading state ---
  const [isScanning, setIsScanning] = useState(false);
  const [scanStatus, setScanStatus] = useState("");
  const [scanProgress, setScanProgress] = useState(0);

  // --- Queue / Favorites state ---
  // scanQueue: array of station objects { id, name, freq } waiting to be scanned, FIFO
  const [scanQueue, setScanQueue] = useState([]);
  // favorites: Set of station ids (by string id) that auto-refill the queue
  const [favorites, setFavorites] = useState(new Set());
  // activeScanFreq: frequency currently being processed by the pipeline (null = idle)
  const [activeScanFreq, setActiveScanFreq] = useState(null);
  // favoritesCursor: rotates through favorites list when auto-refilling the queue
  const favoritesCursorRef = useRef(0);


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

  // --- FIX: Scheduler ref to queue audio chunks back-to-back ---
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

  // Refs for WebSocket listener to read current values without stale closures
  const stationsRef = useRef([]);
  const favoritesRef = useRef(new Set());
  useEffect(() => { stationsRef.current = stations; }, [stations]);
  useEffect(() => { favoritesRef.current = favorites; }, [favorites]);

  const fetchLogs = async () => {
    try {
      const res = await fetch('/api/logs'); 
      const data = await res.json();
      setLogs(data);
      setIsSearchActive(false);
      setSearchTerm('');
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

  const handleAgentSubmit = async (e) => {
    e.preventDefault();
    if (!agentInput.trim() || isAgentThinking) return;

    const userText = agentInput.trim();
    setAgentInput("");
    setIsAgentThinking(true);

    // Keep only the last 2 interactions (4 messages) to save tokens
    const recentHistory = agentMessages.slice(-4);
    const newMessages = [...recentHistory, { role: "user", content: userText }];
    
    // Optimistically update UI
    setAgentMessages([...agentMessages, { role: "user", content: userText }]);

    try {
      const res = await fetch('/api/agent/chat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ messages: newMessages })
      });
      const data = await res.json();
      
      if (data.answer) {
        setAgentMessages(prev => [...prev, { role: "assistant", content: data.answer }]);
      } else {
        setAgentMessages(prev => [...prev, { role: "assistant", content: "Error: Agent failed to respond." }]);
      }
    } catch (err) {
      setAgentMessages(prev => [...prev, { role: "assistant", content: "Connection error." }]);
    } finally {
      setIsAgentThinking(false);
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
      // FIX: Force the AudioContext to match the backend 16kHz output
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

      // FIX: Trigger the LIVE_LISTEN command on the backend explicitly
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
      
      // FIX: Ensure backend terminates the live listen stream when off
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
      // HARDWIRED to explicitly ask the C++ backend for audio.wav
      //const fetchPath = "http://localhost:8080/api/audio/audio.wav";
       const fetchPath = `http://localhost:8080${selectedLog.audioFilePath}`;

      
      fetch(fetchPath)
        .then(res => {
          if (!res.ok) throw new Error("File not found on backend!");
          return res.blob();
        })
        .then(blob => {
          // Force the blob to be read as a wav file
          const wavBlob = new Blob([blob], { type: 'audio/wav' });
          objectUrl = URL.createObjectURL(wavBlob);
          setLocalAudioUrl(objectUrl);
        })
        .catch(err => {
          console.error("Playback Error:", err.message);
          alert("Could not load audio.wav from the backend.");
        });
    }

    return () => {
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [showPlaybackMenu, selectedLog]);

  // THE ASYNC PIPELINE: WebSocket listener drives transcription + summarization.
  // Progress is stored per-frequency in scanJobs so switching stations works.
  // On summary_complete: auto-save if favorited, then clear activeScanFreq
  // so the queue processor can pick the next scan.
  useEffect(() => {
    const statusWs = new WebSocket(`ws://${window.location.host}/ws/status`);
    
    statusWs.onmessage = (event) => {
      const data = JSON.parse(event.data);
      const freq = data.freq;

      if (data.event === "record_complete") {
        
        const generatedFilename = data.file.split('/').pop();
        const apiAudioPath = `/api/audio/${generatedFilename}`;

        setScanProgress(50);
        setScanStatus("AI: Transcribing audio...");
        updateJob(freq, { 
            summary: "AI: Processing transcription...",
            audioFilePath: apiAudioPath 
        });
        
        //Add the file property to the JSON body
        fetch(useOpenAI ? '/api/transcribe/openai' : '/api/transcribe/local', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ 
              freq: parseFloat(freq),
              file: generatedFilename // <--- Pass the filename to the API!
          }),
        });
      }
      else if (data.event === "transcription_complete") {
        updateJob(freq, { 
          progress: 75,
          statusText: "AI: Generating summary...",
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
        updateJob(freq, { 
          progress: 100,
          statusText: "Scan complete!",
          status: "complete",
          summary: data.summary 
        });

        // Auto-save if this station is favorited (check by finding the station)
        // We look up the station via the stations list so we have name/id.
        setScanJobs(prevJobs => {
          const job = prevJobs[freq];
          if (job) {
            // Find matching station (might be in main list or from a scanned queue entry)
            const station = stationsRef.current.find(s => parseFloat(s.freq) === parseFloat(freq));
            if (station && favoritesRef.current.has(station.id)) {
              // Fire auto-save (fire-and-forget)
              fetch('/api/logs/save', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                  freq: parseFloat(freq),
                  time: Math.floor(Date.now() / 1000),
                  location: "Birmingham, AL",
                  rawT: job.rawText,
                  summary: data.summary,
                  channelName: station.name
                }),
              }).then(res => {
                if (res.ok) {
                  console.log(`[Auto-save] Saved favorited station ${station.name}`);
                  fetchLogs();
                }
              }).catch(err => console.error("Auto-save failed:", err));
            }
          }
          return prevJobs;
        });

        // Brief pause so "100% / Scan complete!" is visible, then clear the
        // bar on this freq and release the pipeline for the next queue item.
        setTimeout(() => {
          updateJob(freq, { progress: 0, statusText: "" });
          setActiveScanFreq(null);
        }, 1200);
      }
    };

    return () => statusWs.close();
  }, [useOpenAI]);


  // ENQUEUE: adds a station to the queue. The processor effect below
  // will pick it up when the pipeline is idle.
  const handleScan = () => {
    if (!selectedStation) return;
    enqueueStation(selectedStation);
  };

    // 1. Generate the master timestamp exactly ONCE
    const scanTimestamp = Math.floor(Date.now() / 1000);

  const enqueueStation = (station) => {
    setScanQueue(prev => {
      // Prevent duplicate queue entries for the same freq
      if (prev.some(q => parseFloat(q.freq) === parseFloat(station.freq))) return prev;
      return [...prev, station];
    });
  };

  const removeFromQueue = (freq) => {
    setScanQueue(prev => prev.filter(q => parseFloat(q.freq) !== parseFloat(freq)));
  };

  // QUEUE PROCESSOR: when the pipeline is idle and there's something to scan,
  // dequeue the next station and start its recording. When the queue is empty
  // but there are favorites, cycle to the next favorite and enqueue it.
  useEffect(() => {
    if (activeScanFreq !== null) return; // pipeline busy

    // Queue has entries → start the next one
    if (scanQueue.length > 0) {
      const next = scanQueue[0];
      setScanQueue(prev => prev.slice(1));
      runScan(next);
      return;
    }

    // Queue empty but favorites exist → cycle and enqueue
    if (favorites.size > 0) {
      const favStations = stations.filter(s => favorites.has(s.id));
      if (favStations.length > 0) {
        const idx = favoritesCursorRef.current % favStations.length;
        favoritesCursorRef.current = (idx + 1) % favStations.length;
        const nextFav = favStations[idx];
        setScanQueue([nextFav]);
      }
    }
  }, [activeScanFreq, scanQueue, favorites, stations]);

  // RUN SCAN: kicks off the recording for a single station. The WebSocket
  // listener handles transcription → summarization → auto-save → release.
  const runScan = async (station) => {
    const targetFreq = station.freq;
    setActiveScanFreq(parseFloat(targetFreq));

    // 2. Save it to the job state so we can use it later when saving
    updateJob(targetFreq, { 
      status: "recording",
      progress: 5,
      statusText: `Connecting to ${Number(targetFreq).toFixed(3)} MHz...`,
      summary: "Hardware: Recording 30-second capture...",
      rawText: `Capturing audio from ${Number(targetFreq).toFixed(3)} MHz...`,
      timestamp: scanTimestamp // <--- MUST BE HERE
    });

    try {
      updateJob(targetFreq, { progress: 10, statusText: "Hardware: Starting 30-second recording..." });
      
      // 3. SEND THE TIMESTAMP TO THE HARDWARE!
      const recordRes = await fetch('/api/scan/record', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ 
          freq: parseFloat(targetFreq),
          timestamp: scanTimestamp // <--- MISSING LINK ADDED HERE
        })
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

      updateJob(targetFreq, { summary: "Hardware: Background recording in progress (30s)..." });

      // Animate progress over the 30-second window
      const recordingStart = Date.now();
      const recordingDuration = 31000;
      const progressInterval = setInterval(() => {
        const elapsed = Date.now() - recordingStart;
        const ratio = Math.min(elapsed / recordingDuration, 1);
        const currentProgress = Math.round(10 + ratio * 35); // 10% → 45%
        const secondsLeft = Math.max(0, Math.ceil((recordingDuration - elapsed) / 1000));

        updateJob(targetFreq, {
          progress: currentProgress,
          statusText: `Hardware: Recording in progress... (${secondsLeft}s remaining)`
        });

        if (ratio >= 1) clearInterval(progressInterval);
      }, 500);

      await new Promise(resolve => setTimeout(resolve, recordingDuration));
      clearInterval(progressInterval);

      updateJob(targetFreq, { progress: 48, statusText: "Waiting for AI pipeline..." });
      // WebSocket listener takes over from here — it will eventually clear activeScanFreq.

    } catch (error) {
      console.error(`Scan error on ${targetFreq}:`, error);
      updateJob(targetFreq, { 
        status: "error",
        progress: 0,
        statusText: `Error: ${error.message}`,
        summary: `Error: ${error.message}`,
        rawText: "Scan failed. Check server console."
      });
      // Release the pipeline so the next queue item can run
      setTimeout(() => setActiveScanFreq(null), 2000);
    }
  };

  const handleSave = async () => {
    if (!selectedStation) {
      alert("Please select a frequency first!");
      return;
    }

    const targetFreq = selectedStation.freq;
    const jobData = scanJobs[targetFreq];

    console.log("audioFilePath being saved:", jobData.audioFilePath);
    console.log("Full jobData:", jobData);

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
          channelName: selectedStation.name,
          audioFilePath: jobData.audioFilePath || `/api/audio/captured_${jobData.timestamp}.wav`        }),
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

  const toggleFavorite = (stationId) => {
    setFavorites(prev => {
      const next = new Set(prev);
      if (next.has(stationId)) next.delete(stationId);
      else next.add(stationId);
      return next;
    });
  };

  // UI DERIVATIONS (Extracting the current job status for the UI dynamically)
  const currentJob = selectedStation ? scanJobs[selectedStation.freq] : null;
  const displaySummary = currentJob ? currentJob.summary : (isScanningBand ? "Hardware: Performing 88-108 MHz sweep..." : "Waiting for scan...");
  const displayRawText = currentJob ? currentJob.rawText : "";
  // Progress bar shows only when the *currently viewed* station has an active scan
  const showProgressBar = currentJob && currentJob.progress > 0 && currentJob.status !== "complete"
    || (currentJob && currentJob.progress === 100);
  const currentProgress = currentJob?.progress || 0;
  const currentStatusText = currentJob?.statusText || "";

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

      {/* FLOATING AGENT WIDGET */}
      {view === 'database' && (
        <div style={{
          position: 'fixed', bottom: '20px', right: '20px', width: '350px',
          backgroundColor: '#001a33', border: '1px solid #004080', borderRadius: '8px',
          boxShadow: '0 8px 16px rgba(0,0,0,0.5)', zIndex: 1000,
          display: 'flex', flexDirection: 'column', overflow: 'hidden'
        }}>
          {/* Header (Click to toggle) */}
          <div 
            onClick={() => setIsAgentOpen(!isAgentOpen)}
            style={{ padding: '10px 15px', backgroundColor: '#002b5e', cursor: 'pointer', display: 'flex', justifyContent: 'space-between', alignItems: 'center', fontWeight: 'bold' }}
          >
            <span>AetherGuard Agent</span>
            <span>{isAgentOpen ? '▼' : '▲'}</span>
          </div>

          {/* Body */}
          {isAgentOpen && (
            <div style={{ height: '350px', display: 'flex', flexDirection: 'column' }}>
              {!useOpenAI ? (
                <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#8cb4d5', padding: '20px', textAlign: 'center' }}>
                  Database agent disabled for local model. Switch to OpenAI to enable.
                </div>
              ) : (
                <>
                  <div style={{ flex: 1, overflowY: 'auto', padding: '15px', display: 'flex', flexDirection: 'column', gap: '10px' }}>
                    {agentMessages.length === 0 && (
                      <div style={{ color: '#5a8aad', fontSize: '0.85em', textAlign: 'center' }}>Ask me about the database records.</div>
                    )}
                    {agentMessages.map((msg, idx) => (
                      <div key={idx} style={{ alignSelf: msg.role === 'user' ? 'flex-end' : 'flex-start', backgroundColor: msg.role === 'user' ? '#004080' : 'rgba(255,255,255,0.05)', padding: '8px 12px', borderRadius: '6px', maxWidth: '85%', fontSize: '0.9em', color: 'white' }}>
                        {msg.content}
                      </div>
                    ))}
                    {isAgentThinking && (
                      <div style={{ alignSelf: 'flex-start', color: '#8cb4d5', fontSize: '0.85em', fontStyle: 'italic' }}>Agent is thinking...</div>
                    )}
                  </div>
                  <form onSubmit={handleAgentSubmit} style={{ display: 'flex', padding: '10px', borderTop: '1px solid #004080', backgroundColor: 'rgba(0,0,0,0.2)' }}>
                    <input 
                      type="text" 
                      value={agentInput} 
                      onChange={(e) => setAgentInput(e.target.value)} 
                      placeholder="Search the logs..." 
                      style={{ flex: 1, padding: '8px', borderRadius: '4px', border: '1px solid #333', backgroundColor: '#001a33', color: 'white' }}
                      disabled={isAgentThinking}
                    />
                    <button type="submit" disabled={isAgentThinking} style={{ marginLeft: '8px', padding: '8px 12px', backgroundColor: '#3b82f6', color: 'white', border: 'none', borderRadius: '4px', cursor: isAgentThinking ? 'not-allowed' : 'pointer' }}>
                      Send
                    </button>
                  </form>
                </>
              )}
            </div>
          )}
        </div>
      )}

 {/* SCANNING VIEW */}
      {view === 'scanning' && (
        <div className="scanning-container">
          <div className="scanning-grid" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 0.55fr', gap: '20px', alignItems: 'start' }}>
            <div className="data-box">
              <h3 style={{ marginBottom: '15px' }}>Live Frequencies</h3>
              
              <button 
                className="sub-btn" 
                onClick={handleWidebandSweep}
                disabled={isScanningBand || activeScanFreq !== null}
                style={{ 
                  width: '100%',      
                  marginBottom: '15px', 
                  padding: '12px',
                  backgroundColor: (isScanningBand || activeScanFreq !== null) ? '#f39c12' : '#3b82f6',
                  color: 'white',
                  fontWeight: 'bold',
                  display: 'flex',
                  justifyContent: 'center',
                  alignItems: 'center',
                  border: 'none',
                  borderRadius: '6px',
                  cursor: (isScanningBand || activeScanFreq !== null) ? 'not-allowed' : 'pointer',
                  transition: 'background-color 0.3s ease'
                }}
              >
                {isScanningBand ? "Sweeping Spectrum (88-108 MHz)..." : 
                 activeScanFreq !== null ? "Scan in progress..." : "Auto-Find Stations"}
              </button>

              {/* Zeroed out default browser list spacing and made it scrollable */}
              <ul className="frequency-list" style={{ height: '400px', overflowY: 'auto', marginTop: 0, paddingLeft: 0, paddingRight: '5px', listStyle: 'none' }}>
                {stations.map(s => (
                  <li 
                    key={s.id}
                    onClick={() => {
                      setSelectedStation(s);
                      setIsListeningLive(false);
                    }}
                    className={selectedStation?.id === s.id ? "active-station" : ""}
                  >
                    <div className="station-item-content" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                      <div style={{ display: 'flex', alignItems: 'center', flex: 1, minWidth: 0 }}>
                        <span className="freq-tag">{Number(s.freq).toFixed(3)} MHz</span>
                        <span className="station-name">{s.name}</span>
                      </div>
                      <button
                        onClick={(e) => {
                          e.stopPropagation();
                          toggleFavorite(s.id);
                        }}
                        title={favorites.has(s.id) ? "Unfavorite (stop auto-cycling)" : "Favorite (auto-cycle when queue is empty)"}
                        style={{
                          background: 'none',
                          border: 'none',
                          cursor: 'pointer',
                          fontSize: '1.3em',
                          color: favorites.has(s.id) ? '#f1c40f' : '#555',
                          padding: '0 8px',
                          transition: 'color 0.2s',
                        }}
                      >
                        {favorites.has(s.id) ? '★' : '☆'}
                      </button>
                    </div>
                  </li>
                ))}
              </ul>
            </div>
 
            <div className="data-box">
              <h3>Transmission Summary</h3>
              <div className="summary-content">
                {/* Progress bar shows when the currently viewed station has an active scan */}
                {showProgressBar && <ScanProgress status={currentStatusText} progress={currentProgress} />}
                
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
                {/* Listen Live — allowed any time except when a *different* station is mid-scan */}
                <button 
                  className="sub-btn" 
                  style={{ 
                    backgroundColor: isListeningLive ? '#e74c3c' : '#f39c12',
                    color: 'white' 
                  }}
                  onClick={() => setIsListeningLive(!isListeningLive)} 
                  disabled={
                    !selectedStation ||
                    (activeScanFreq !== null && parseFloat(selectedStation.freq) !== activeScanFreq)
                  }
                >
                  {isListeningLive ? "Stop Live Audio" : "Listen Live"}
                </button>

                <button 
                  className="sub-btn scan-btn" 
                  onClick={handleScan} 
                  disabled={!selectedStation}
                >
                  {currentJob && currentJob.status === "recording" ? "Scanning..." : "Scan"}
                </button>
                
                <button 
                  className="sub-btn save-btn" 
                  onClick={handleSave} 
                  disabled={!selectedStation || currentJob?.status !== 'complete'}
                >
                  Save
                </button>
              </div>
            </div>

            {/* QUEUE COLUMN: Progress bar top (25%), gap (5%), queue list bottom (70%) */}
            <div className="data-box" style={{ display: 'flex', flexDirection: 'column', height: '480px' }}>
              {/* TOP 25%: Active pipeline status */}
              <div style={{ 
                flex: '0 0 25%',
                minHeight: 0,
                display: 'flex',
                flexDirection: 'column',
                justifyContent: 'flex-start',
                borderBottom: '1px solid #222',
                paddingBottom: '8px',
              }}>
                <h3 style={{ marginTop: 0, marginBottom: '10px', fontSize: '1em' }}>Pipeline</h3>
                {activeScanFreq !== null ? (
                  <>
                    <p style={{ margin: '0 0 6px 0', fontSize: '0.85em', color: '#8cb4d5' }}>
                      Scanning {Number(activeScanFreq).toFixed(3)} MHz
                    </p>
                    <p style={{ margin: 0, fontSize: '0.75em', color: '#5a8aad' }}>
                      {scanJobs[activeScanFreq]?.statusText || "Working..."}
                    </p>
                  </>
                ) : (
                  <p style={{ margin: 0, fontSize: '0.85em', color: '#666' }}>
                    Pipeline idle
                  </p>
                )}
              </div>

              {/* 5% GAP */}
              <div style={{ flex: '0 0 5%' }} />

              {/* BOTTOM 70%: Queue */}
              <div style={{ 
                flex: '0 0 70%',
                minHeight: 0,
                display: 'flex',
                flexDirection: 'column',
              }}>
                <h3 style={{ marginTop: 0, marginBottom: '10px', fontSize: '1em' }}>
                  Queue ({scanQueue.length})
                </h3>
                <ul style={{ 
                  flex: 1,
                  overflowY: 'auto',
                  margin: 0,
                  padding: 0,
                  listStyle: 'none',
                }}>
                  {scanQueue.length === 0 ? (
                    <li style={{ color: '#555', fontSize: '0.85em', fontStyle: 'italic', padding: '8px 4px' }}>
                      {favorites.size > 0 
                        ? "Cycling favorites..." 
                        : "Empty — click Scan or favorite (★) stations to add."}
                    </li>
                  ) : (
                    scanQueue.map((q, idx) => (
                      <li key={`${q.id}-${idx}`} style={{
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'space-between',
                        padding: '6px 8px',
                        marginBottom: '4px',
                        backgroundColor: 'rgba(0,43,94,0.2)',
                        borderRadius: '4px',
                        fontSize: '0.85em',
                      }}>
                        <div style={{ flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                          <span style={{ color: '#8cb4d5', fontWeight: 'bold' }}>
                            {Number(q.freq).toFixed(3)}
                          </span>
                          <span style={{ color: '#aaa', marginLeft: '6px' }}>{q.name}</span>
                        </div>
                        <button
                          onClick={() => removeFromQueue(q.freq)}
                          title="Remove from queue"
                          style={{
                            background: 'none',
                            border: 'none',
                            color: '#e74c3c',
                            cursor: 'pointer',
                            fontSize: '1em',
                            padding: '0 6px',
                          }}
                        >
                          ✕
                        </button>
                      </li>
                    ))
                  )}
                </ul>
              </div>
            </div>
          </div>
          <div className="button-container">
            <button className="back-btn" onClick={resetView}>Back to Home</button>
          </div>
        </div>
      )}
    </div>
  );
}
 
export default App