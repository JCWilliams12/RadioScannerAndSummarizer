import { useState, useEffect } from "react";

const blips = [
  { top: "22%", left: "68%", delay: 1.5, size: 6 },
  { top: "35%", left: "25%", delay: 3.2, size: 5 },
  { top: "70%", left: "72%", delay: 2.1, size: 7 },
  { top: "62%", left: "30%", delay: 4.0, size: 5 },
  { top: "28%", left: "48%", delay: 2.8, size: 4 },
];

const keyframes = `
  @keyframes expandPulse {
    0% { transform: translate(-50%, -50%) scale(0.08); opacity: 0.7; border-color: rgba(0,229,255,0.6); }
    70% { opacity: 0.15; border-color: rgba(0,229,255,0.15); }
    100% { transform: translate(-50%, -50%) scale(1); opacity: 0; border-color: rgba(0,229,255,0); }
  }
  @keyframes wifiPulse {
    0%, 100% { transform: scale(1); filter: drop-shadow(0 0 6px rgba(0,229,255,0.4)); }
    50% { transform: scale(1.08); filter: drop-shadow(0 0 14px rgba(0,229,255,0.7)); }
  }
  @keyframes arcFade {
    0%, 100% { opacity: 0.3; }
    50% { opacity: 1; }
  }
  @keyframes blipPing {
    0%, 20% { opacity: 0; transform: scale(0); }
    30% { opacity: 1; transform: scale(1.6); }
    50% { opacity: 1; transform: scale(1); }
    80% { opacity: 0.6; }
    100% { opacity: 0; transform: scale(0.5); }
  }
  @keyframes labelPulse {
    0%, 100% { opacity: 0.5; }
    50% { opacity: 1; }
  }
  @keyframes dotBlink {
    0%, 50% { opacity: 0; }
    50.01%, 100% { opacity: 1; }
  }
  @keyframes hubGlow {
    0%, 100% { box-shadow: 0 0 20px rgba(0,229,255,0.15), 0 0 40px rgba(0,229,255,0.05), inset 0 0 15px rgba(0,229,255,0.08); }
    50% { box-shadow: 0 0 30px rgba(0,229,255,0.3), 0 0 60px rgba(0,229,255,0.1), inset 0 0 20px rgba(0,229,255,0.15); }
  }
`;

export default function WifiSearchAnimation() {
  const [scanAngle, setScanAngle] = useState(0);

  useEffect(() => {
    let frame;
    const animate = () => {
      setScanAngle((prev) => (prev + 1.2) % 360);
      frame = requestAnimationFrame(animate);
    };
    frame = requestAnimationFrame(animate);
    return () => cancelAnimationFrame(frame);
  }, []);

  return (
    <div style={{
      minHeight: "100vh",
      display: "flex",
      justifyContent: "center",
      alignItems: "center",
      background: "linear-gradient(145deg, #030810 0%, #0a1628 50%, #060d18 100%)",
      fontFamily: "'SF Mono', 'Fira Code', 'JetBrains Mono', monospace",
    }}>
      <style>{keyframes}</style>

      <div style={{
        position: "relative",
        width: "420px",
        height: "420px",
        display: "flex",
        justifyContent: "center",
        alignItems: "center",
        background: "radial-gradient(circle at center, #0a1628 0%, #060d18 60%, #030810 100%)",
        borderRadius: "50%",
        overflow: "hidden",
      }}>

        {/* Ambient glow */}
        <div style={{
          position: "absolute", width: "100%", height: "100%", borderRadius: "50%",
          background: "radial-gradient(circle, rgba(0,229,255,0.03) 0%, transparent 70%)",
          pointerEvents: "none",
        }} />

        {/* Grid rings */}
        {[1, 2, 3, 4].map((i) => (
          <div key={i} style={{
            position: "absolute", top: "50%", left: "50%",
            transform: "translate(-50%, -50%)", borderRadius: "50%",
            width: `${i * 100}px`, height: `${i * 100}px`,
            border: "1px solid rgba(0,229,255,0.07)", pointerEvents: "none",
          }} />
        ))}

        {/* Crosshairs */}
        <div style={{ position: "absolute", width: "100%", height: "1px", top: "50%", left: 0, background: "rgba(0,229,255,0.04)" }} />
        <div style={{ position: "absolute", width: "1px", height: "100%", top: 0, left: "50%", background: "rgba(0,229,255,0.04)" }} />

        {/* Sweep beam */}
        <div style={{
          position: "absolute", width: "100%", height: "100%", top: 0, left: 0,
          transformOrigin: "center center", transform: `rotate(${scanAngle}deg)`, pointerEvents: "none",
        }}>
          <div style={{
            position: "absolute", top: "0%", left: "50%", width: "50%", height: "50%",
            transformOrigin: "bottom left",
            background: "conic-gradient(from -30deg at 0% 100%, transparent 0deg, rgba(0,229,255,0.12) 25deg, transparent 55deg)",
            borderRadius: "0 100% 0 0",
          }} />
        </div>

        {/* Pulse waves */}
        {[0, 1, 2, 3].map((i) => (
          <div key={i} style={{
            position: "absolute", top: "50%", left: "50%", width: "380px", height: "380px",
            borderRadius: "50%", border: "1.5px solid rgba(0,229,255,0.5)",
            animation: `expandPulse 4.4s ease-out infinite ${i * 1.1}s`, pointerEvents: "none",
          }} />
        ))}

        {/* Signal blips */}
        {blips.map((b, i) => (
          <div key={i} style={{
            position: "absolute", borderRadius: "50%", top: b.top, left: b.left,
            width: `${b.size}px`, height: `${b.size}px`,
            background: "rgba(0,229,255,0.9)",
            boxShadow: "0 0 8px rgba(0,229,255,0.6), 0 0 20px rgba(0,229,255,0.2)",
            animation: `blipPing 5s ease-in-out infinite ${b.delay}s`, pointerEvents: "none",
          }} />
        ))}

        {/* Center hub + Wi-Fi icon */}
        <div style={{
          position: "relative", zIndex: 10, width: "72px", height: "72px", borderRadius: "50%",
          background: "radial-gradient(circle, rgba(10,25,50,0.95) 0%, rgba(6,15,30,1) 100%)",
          border: "1.5px solid rgba(0,229,255,0.2)", display: "flex",
          justifyContent: "center", alignItems: "center", animation: "hubGlow 3s ease-in-out infinite",
        }}>
          <svg width="38" height="38" viewBox="0 0 24 24" fill="none"
            style={{ animation: "wifiPulse 2s ease-in-out infinite" }}>
            <path d="M12 18.5a1.5 1.5 0 110 3 1.5 1.5 0 010-3z" fill="#00e5ff"
              style={{ animation: "arcFade 2.4s ease-in-out infinite" }} />
            <path d="M8.11 15.53a5.46 5.46 0 017.78 0" stroke="#00e5ff" strokeWidth="2" strokeLinecap="round"
              style={{ animation: "arcFade 2.4s ease-in-out infinite 0.15s", opacity: 0.85 }} />
            <path d="M4.93 12.35a10 10 0 0114.14 0" stroke="#00e5ff" strokeWidth="2" strokeLinecap="round"
              style={{ animation: "arcFade 2.4s ease-in-out infinite 0.3s", opacity: 0.65 }} />
            <path d="M1.75 9.17a14.48 14.48 0 0120.5 0" stroke="#00e5ff" strokeWidth="2" strokeLinecap="round"
              style={{ animation: "arcFade 2.4s ease-in-out infinite 0.45s", opacity: 0.4 }} />
          </svg>
        </div>

        {/* Label */}
        <div style={{
          position: "absolute", bottom: "52px", color: "rgba(0,229,255,0.6)",
          fontSize: "11px", letterSpacing: "2.5px", textTransform: "uppercase",
          fontFamily: "'SF Mono', 'Fira Code', monospace", display: "flex", alignItems: "center", gap: "1px",
        }}>
          <span style={{ animation: "labelPulse 2.5s ease-in-out infinite" }}>Searching for networks</span>
          <span style={{ display: "inline-flex", width: "20px", letterSpacing: "1px" }}>
            <span style={{ animation: "dotBlink 1.4s steps(1) infinite 0s" }}>.</span>
            <span style={{ animation: "dotBlink 1.4s steps(1) infinite 0.3s" }}>.</span>
            <span style={{ animation: "dotBlink 1.4s steps(1) infinite 0.6s" }}>.</span>
          </span>
        </div>
      </div>
    </div>
  );
}
