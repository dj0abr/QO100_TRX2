"""Small reliable request/reply layer for the backend's settings snapshot."""
import threading
import time
from protocol import settings_control, settings_state


class SettingsClient:
    def __init__(self):
        self.lock=threading.RLock()
        self.request=1
        self.session=0
        self.revision=0
        self.flags=0
        self.values=None
        self.ready=False
        self.patch=None
        self.ticket=None
        self.results={}
        self.preferences={}
        self.next_send=0
        self.pending=False
        self.error=''
        self.retries=0
        self.last_reply=time.monotonic()

    def reset(self):
        with self.lock:
            self.request=(self.request+1)&0xffffffff
            self.session=0;self.values=None;self.patch=None;self.pending=False
            self.ready=False
            self.preferences.clear();self.next_send=0

    def set(self, patch):
        with self.lock:
            if self.patch is not None or self.pending: raise RuntimeError('Einstellungen werden bereits übernommen')
            self.request=(self.request+1)&0xffffffff
            self.patch={**self.preferences,**{k:str(v) for k,v in patch.items()}}
            self.preferences.clear()
            self.ticket=self.request;self.retries=0;self.next_send=0
            self.pending=True
            return self.ticket

    def save_preferences(self, **patch):
        with self.lock:
            for k,v in patch.items():
                if self.values is not None:
                    old=self.values.get(k)
                    try: same=float(v)==float(old)
                    except (TypeError,ValueError): same=str(v)==old
                    if not same: self.preferences[k]=str(v)

    def send(self, sock):
        with self.lock:
            if self.values is not None and self.patch is None and not self.pending and self.preferences:
                self.set(self.preferences.copy())
            if time.monotonic()<self.next_send: return
            probe=self.patch is not None and time.monotonic()-self.last_reply>2
            sock.send(settings_control(self.request,0 if self.patch is None or probe else self.session,
                                       self.revision,None if probe else self.patch))
            self.next_send=time.monotonic()+(.25 if self.patch is not None or self.values is None else 1.)

    def receive(self, packet):
        result=settings_state(packet)
        if result is None: return None
        request,session,revision,status,flags,values=result
        with self.lock:
            if request!=self.request: return None
            if self.session==session and revision<self.revision: return None
            if self.session and session!=self.session:
                if self.patch is not None: self.results[self.ticket]=(6,'Backend neu gestartet; Einstellungen erneut prüfen')
                self.patch=None;self.values=None;self.ready=False;self.preferences.clear()
            self.last_reply=time.monotonic()
            first=not self.ready
            self.session=session;self.revision=revision;self.values=values;self.flags=flags
            self.pending=status==4 or bool(flags&2)
            if not self.pending: self.ready=True
            self.error=values.get('error',values.get('hardware_error',''))
            if self.patch is not None and not self.pending:
                if status==2 and self.retries<3:
                    self.retries+=1;self.request=(self.request+1)&0xffffffff;self.next_send=0;self.pending=True
                else:
                    self.results[self.ticket]=(status,self.error)
                    while len(self.results)>16: del self.results[next(iter(self.results))]
                    self.patch=None;self.request=(self.request+1)&0xffffffff;self.next_send=0
            return values if first and self.ready else None
