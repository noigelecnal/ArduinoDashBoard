import { useEffect, useRef, useState } from 'react'
import { ExternalLink } from 'lucide-react'
import './TouchscreenView.css'

// The touchscreen UI is a standalone page in /public/touchscreen (480 x 320 panel plus its bezel).
const SRC = `${import.meta.env.BASE_URL}touchscreen/index.html`
const FRAME_W = 560
const FRAME_H = 400
const MAX_SCALE = 1.5

export default function TouchscreenView() {
  const stageRef = useRef(null)
  const [scale, setScale] = useState(1)

  // Scale the fixed-size panel to the space available (shrinks on phones, grows on desktop).
  useEffect(() => {
    const el = stageRef.current
    if (!el) return undefined
    const update = () => setScale(Math.min(MAX_SCALE, Math.max(0.5, el.clientWidth / FRAME_W)))
    update()
    const ro = new ResizeObserver(update)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  return (
    <section className="panel touch-view" aria-labelledby="touch-title">
      <div className="panel-head touch-head">
        <div>
          <h2 id="touch-title" className="panel-title">Touchscreen interface</h2>
          <p>480 × 320 landscape panel · tap and hold the + / − buttons · same readings and setpoints as the dashboard</p>
        </div>
        <a className="btn ghost" href={SRC} target="_blank" rel="noreferrer">
          <ExternalLink size={16} aria-hidden /> Open full screen
        </a>
      </div>

      <div className="touch-stage" ref={stageRef}>
        <div className="touch-frame" style={{ width: FRAME_W * scale, height: FRAME_H * scale }}>
          <iframe
            title="BioPod touchscreen interface"
            src={SRC}
            style={{ width: FRAME_W, height: FRAME_H, transform: `scale(${scale})` }}
          />
        </div>
      </div>

      <p className="note">
        With no ESP32 connected the screen switches to demo readings after a few seconds, like the dashboard's demo mode.
      </p>
    </section>
  )
}
