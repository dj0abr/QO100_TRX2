"""QO-100 frequency mapping and the geometry shared by drawing and mouse tuning."""
from dataclasses import dataclass

DOWNLINK_UPLINK_HZ = 8_089_500_000


def uplink_hz(downlink_hz, offset_hz=0):
    return downlink_hz - DOWNLINK_UPLINK_HZ + offset_hz


def tx_offset_hz(rx_hz, tx_hz):
    return tx_hz - uplink_hz(rx_hz)


def mhz_text(hz):
    return f"{hz//1_000_000}.{hz%1_000_000:06d}"


@dataclass(frozen=True)
class PlotGeometry:
    center_hz: int
    span_hz: int
    width: int
    height: int

    @property
    def left(self): return 70

    @property
    def right(self): return self.width-20

    @property
    def spectrum_top(self): return 60

    @property
    def spectrum_bottom(self): return int(self.height*.45)

    @property
    def waterfall_top(self): return int(self.height*.55)

    @property
    def bottom(self): return self.height-15

    def x(self, hz):
        return self.left + (hz-self.center_hz+self.span_hz/2)*(self.right-self.left)/self.span_hz

    def frequency_at(self, x, y):
        if not (self.left <= x < self.right and
                (self.spectrum_top <= y <= self.spectrum_bottom or self.waterfall_top <= y < self.bottom)):
            return None
        return round(self.center_hz-self.span_hz/2 + (x-self.left)*self.span_hz/(self.right-self.left))

    def widget_frequency_at(self, x, y, widget_width, widget_height):
        # Tk centers its fixed-size image inside the label, also after resize.
        return self.frequency_at(x-(widget_width-self.width)/2, y-(widget_height-self.height)/2)


def wheel_steps(event):
    if getattr(event,'num',None) == 4: return 1
    if getattr(event,'num',None) == 5: return -1
    delta=getattr(event,'delta',0)
    if not delta: return 0
    # X11 uses buttons 4/5, Windows multiples of 120, macOS small deltas.
    return round(delta/120) if abs(delta)>=120 else (1 if delta>0 else -1)
