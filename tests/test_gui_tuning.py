"""RF mapping and image coordinates used by the two mouse-tunable waterfalls."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'gui'))
from tuning import PlotGeometry, uplink_hz, tx_offset_hz, mhz_text, wheel_steps


class MouseTuningTest(unittest.TestCase):
    def test_station_frequencies_and_offset(self):
        self.assertEqual(uplink_hz(10489750000),2400250000)
        self.assertEqual(uplink_hz(10489900000),2400400000)
        self.assertEqual(uplink_hz(10489900000,123),2400400123)
        self.assertEqual(tx_offset_hz(10489900000,2400400123),123)
        self.assertEqual(mhz_text(10489748500),'10489.748500')

    def test_full_and_zoom_center_and_scale(self):
        wide=PlotGeometry(10489750000,600000,1100,265)
        zoom=PlotGeometry(10489900000,50000,1100,265)
        for geometry in (wide,zoom):
            for y in (80,200):
                self.assertEqual(geometry.frequency_at(575,y),geometry.center_hz)
                self.assertEqual(geometry.frequency_at(322.5,y),geometry.center_hz-geometry.span_hz//4)
                self.assertEqual(geometry.frequency_at(827.5,y),geometry.center_hz+geometry.span_hz//4)
            # The image stays centered when its Tk label gets resized.
            self.assertEqual(geometry.widget_frequency_at(675,250,1300,365),geometry.center_hz)
            for x,y in ((69,200),(1080,200),(575,10),(575,135),(575,260)):
                self.assertIsNone(geometry.frequency_at(x,y))
        self.assertEqual(zoom.frequency_at(zoom.x(10489901000),200),10489901000)

    def test_wheel_platform_events(self):
        for event,steps in ((SimpleNamespace(num=4),1),(SimpleNamespace(num=5),-1),
                            (SimpleNamespace(delta=120),1),(SimpleNamespace(delta=-240),-2),
                            (SimpleNamespace(delta=1),1),(SimpleNamespace(delta=-1),-1),
                            (SimpleNamespace(delta=0),0)):
            self.assertEqual(wheel_steps(event),steps)


if __name__=='__main__': unittest.main()
