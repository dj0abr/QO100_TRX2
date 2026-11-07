export class RadioAudio {
  constructor(onSamples, onChange) {
    this.onSamples = onSamples; this.onChange = onChange;
    this.gain = 4; this.enabled = false; this.micGeneration = 0;
    this.devices = []; this.error = ''; this.warning = ''; this.inputId = ''; this.outputId = '';
    try { this.inputId = localStorage.getItem('qo100.audioInput') || ''; this.outputId = localStorage.getItem('qo100.audioOutput') || ''; } catch {}
    navigator.mediaDevices?.addEventListener('devicechange', () => this.refreshDevices().catch(() => {}));
  }
  async start() {
    if (this.starting) return this.starting;
    this.error = '';
    this.starting = (async () => {
      if (!window.isSecureContext) throw new Error('Audio benötigt HTTPS oder http://127.0.0.1:8080/ auf dem Rechner des Adapters.');
      if (!this.context) {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (!AudioContextClass) throw new Error('Dieser Browser unterstützt Web Audio nicht.');
        // Use the sound device's native rate; the worklet resamples to/from 12 kHz.
        this.context = new AudioContextClass({latencyHint: 'interactive'});
      }
      const context = this.context;
      // Resume while still inside the actual user gesture, before loading a module.
      const resumed = this.resume(context);
      resumed.catch(() => {}); // Module loading may fail before we reach the await below.
      if (!context.audioWorklet) throw new Error('AudioWorklet ist nicht verfügbar. HTTPS oder localhost verwenden.');
      if (!this.node) {
        await context.audioWorklet.addModule('/audio-worklet.js');
        this.node = new AudioWorkletNode(context, 'radio-audio', {outputChannelCount: [1]});
        this.node.connect(context.destination);
        this.node.port.onmessage = ({data}) => {
          if (this.captureId && data.generation === this.captureId) this.onSamples(data.samples);
        };
        this.node.onprocessorerror = () => {
          this.stop(); this.context?.close().catch(() => {}); this.context = null; this.node = null;
          this.error = 'Audioverarbeitung abgebrochen. Bitte Audio erneut starten.'; this.onChange();
        };
        context.onstatechange = () => this.onChange();
        this.setGain(this.gain);
      }
      await resumed;
      await this.refreshDevices(); await this.routeOutput();
      if (context.state !== 'running') await this.resume(context);
      this.enabled = true;
    })().catch(error => {
      this.enabled = false; this.stopMic();
      this.context?.close().catch(() => {}); this.context = null; this.node = null;
      this.error = this.errorText(error, 'Wiedergabe');
      error.audioMessage = this.error;
      throw error;
    }).finally(() => {
      this.starting = null; this.onChange();
    });
    this.onChange();
    return this.starting;
  }
  async resume(context) {
    let timer;
    try {
      await Promise.race([context.resume(), new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('Der Browser startet die Audioausgabe nicht. Ton und automatische Wiedergabe für diese Website freigeben und erneut starten.')), 6000);
      })]);
    } finally { clearTimeout(timer); }
  }
  async refreshDevices() {
    const version = this.deviceRefresh = (this.deviceRefresh || 0) + 1;
    const devices = navigator.mediaDevices?.enumerateDevices ? await navigator.mediaDevices.enumerateDevices() : [];
    if (version !== this.deviceRefresh) return;
    this.devices = devices;
    // Before permission, browsers may expose only anonymous/default devices.
    // Never treat that restricted list as evidence that a saved device vanished.
    for (const [kind, field, key] of [['audioinput', 'inputId', 'qo100.audioInput'], ['audiooutput', 'outputId', 'qo100.audioOutput']]) {
      const selected = this[field];
      if (selected && selected !== 'default' && devices.some(d => d.kind === kind && d.label) && !devices.some(d => d.kind === kind && d.deviceId === selected)) {
        this[field] = '';
        try { localStorage.removeItem(key); } catch {}
        this.warning = 'Eine gespeicherte Gerätezuordnung war veraltet und wurde erneuert. USB Advanced Audio Device wird bevorzugt.';
      }
    }
    this.onChange();
  }
  deviceId(kind) {
    const selected = kind === 'audioinput' ? this.inputId : this.outputId;
    if (selected === 'default') return selected;
    if (selected && this.devices.some(d => d.kind === kind && d.deviceId === selected)) return selected;
    if (kind === 'audiooutput' && this.context && !this.context.setSinkId) return 'default';
    return this.devices.find(d => d.kind === kind && !['default', 'communications'].includes(d.deviceId) && /usb.*advanced.*audio/i.test(d.label) && (kind !== 'audioinput' || !/monitor/i.test(d.label)))?.deviceId || 'default';
  }
  deviceLabel(kind) {
    if (kind === 'audioinput' && this.microphone) {
      const track = this.microphone.getAudioTracks()[0], settings = track?.getSettings();
      const actual = this.devices.find(d => d.kind === kind && !['default', 'communications'].includes(d.deviceId) && (d.deviceId === settings?.deviceId || settings?.groupId && d.groupId === settings.groupId));
      if (actual?.label) return actual.label;
      if (track?.label && !/^(standard|default)$/i.test(track.label)) return track.label;
    }
    const id = kind === 'audiooutput' && this.activeOutputId ? this.activeOutputId : this.deviceId(kind);
    return this.devices.find(d => d.kind === kind && d.deviceId === id)?.label || 'Systemstandard';
  }
  async routeOutput() {
    if (!this.context) return;
    let id = this.deviceId('audiooutput');
    const system = this.devices.find(d => d.kind === 'audiooutput' && d.deviceId === 'default');
    const chosen = this.devices.find(d => d.kind === 'audiooutput' && d.deviceId === id);
    // Do not turn a functioning default USB output into a forced opaque sink ID.
    if (!this.outputId && system && chosen && (system.groupId && system.groupId === chosen.groupId || /usb.*advanced.*audio/i.test(system.label) && /usb.*advanced.*audio/i.test(chosen.label))) id = 'default';
    const sink = id === 'default' ? '' : id;
    if (typeof this.context.sinkId === 'string' && this.context.sinkId === sink) { this.activeOutputId = id; return; }
    if (!this.context.setSinkId) {
      this.activeOutputId = 'default';
      if (id !== 'default') this.warning = 'Wiedergabe verwendet den Systemstandard. Die Ausgabe lässt sich in diesem Browser nur über pavucontrol zuordnen.';
      return;
    }
    try {
      await this.context.setSinkId(sink); this.activeOutputId = id;
    } catch (error) {
      // Output selection must not close an otherwise working microphone stream.
      this.warning = `Wiedergabe: Die Gerätezuordnung wurde vom Browser abgelehnt (${error.name}). Der Systemstandard wird verwendet.`;
      this.outputId = ''; this.activeOutputId = 'default';
      try { localStorage.removeItem('qo100.audioOutput'); } catch {}
      if (this.context.sinkId !== '') {
        try { await this.context.setSinkId(''); }
        catch (fallback) { this.warning += ` Die Standardausgabe konnte ebenfalls nicht umgeschaltet werden (${fallback.name}). Bitte die Browser-Ausgabe in pavucontrol prüfen.`; }
      }
    }
  }
  async selectDevice(kind, id) {
    this.error = ''; this.warning = '';
    if (kind === 'audioinput') this.inputId = id; else this.outputId = id;
    try { localStorage.setItem(kind === 'audioinput' ? 'qo100.audioInput' : 'qo100.audioOutput', id); } catch {}
    try {
      if (kind === 'audiooutput' && this.context) await this.routeOutput();
      if (kind === 'audioinput' && this.microphone) { this.stopMic(); await this.enableMic(); }
    } catch (error) { this.error = this.errorText(error, kind === 'audioinput' ? 'Mikrofon' : 'Wiedergabe'); throw error; }
    finally { this.onChange(); }
  }
  errorText(error, target) {
    if (error.audioMessage) return error.audioMessage;
    const detail = ({NotAllowedError: 'Zugriff im Browser blockiert. Mikrofon beziehungsweise Ton in den Website-Einstellungen erlauben.', NotFoundError: 'Der Browser hat keine passende Gerätezuordnung geliefert. Bitte Mikrofonfreigabe und Geräteauswahl prüfen.', NotReadableError: 'Das Audiogerät konnte nicht geöffnet werden. Gerätebelegung und PulseAudio prüfen.', OverconstrainedError: 'Die angeforderte Gerätezuordnung ist nicht mehr gültig. Bitte im Setup neu auswählen.'})[error.name] || error.message;
    return `${target}: ${detail}`;
  }
  async enableMic() {
    if (this.microphone || this.micBusy) return;
    const generation = ++this.micGeneration;
    this.micBusy = true; this.error = ''; this.onChange();
    let stream;
    try {
      if (!navigator.mediaDevices?.getUserMedia) throw new Error('Mikrofonzugriff benötigt HTTPS oder localhost.');
      const requestStream = id => {
        return navigator.mediaDevices.getUserMedia({audio: {echoCancellation: false, noiseSuppression: false, autoGainControl: false, ...(id !== 'default' ? {deviceId: {exact: id}} : {})}});
      };
      await this.refreshDevices();
      if (generation !== this.micGeneration) return;
      // Obtain microphone permission before routing output devices. In Brave,
      // the pre-permission output list may not contain a usable default sink.
      const requested = this.deviceId('audioinput');
      try { stream = await requestStream(requested); }
      catch (error) {
        if (requested === 'default' || !['NotFoundError', 'OverconstrainedError'].includes(error.name)) throw error;
        // A saved ID may change when permission is granted. This is only a
        // permission probe: validate/reopen the chosen device before connecting it.
        if (generation !== this.micGeneration) return;
        stream = await requestStream('default');
      }
      await this.refreshDevices();
      if (generation !== this.micGeneration) { stream.getTracks().forEach(track => track.stop()); return; }
      // Labels are usually exposed only after microphone permission has been granted.
      const preferred = this.deviceId('audioinput');
      const device = this.devices.find(d => d.kind === 'audioinput' && d.deviceId === preferred);
      const matches = candidate => {
        const track = candidate.getAudioTracks()[0], settings = track?.getSettings();
        return settings?.deviceId === preferred || device?.label && device.label === track?.label ||
          device?.groupId && device.groupId === settings?.groupId;
      };
      if (preferred !== 'default' && !matches(stream)) {
        // Keep the first valid stream until a replacement actually opens.
        const replacement = await requestStream(preferred);
        stream.getTracks().forEach(t => t.stop()); stream = replacement;
      }
      if (generation !== this.micGeneration) { stream.getTracks().forEach(track => track.stop()); return; }
      if (preferred !== 'default' && !matches(stream)) {
        const actual = stream.getAudioTracks()[0]?.label || 'unbekanntes Gerät';
        throw new Error(`Der Browser hat „${actual}“ statt „${device?.label || 'gewähltes Mikrofon'}“ geöffnet. Bitte die Mikrofonfreigabe und Geräteauswahl dieser Website im Browser prüfen.`);
      }
      await this.start();
      if (generation !== this.micGeneration) { stream.getTracks().forEach(track => track.stop()); return; }
      this.microphone = stream;
      this.source = this.context.createMediaStreamSource(stream);
      this.lowpass = this.context.createBiquadFilter(); this.lowpass.type = 'lowpass'; this.lowpass.frequency.value = Math.min(5000, this.context.sampleRate * .42); this.lowpass.Q.value = .707;
      this.source.connect(this.lowpass); this.lowpass.connect(this.node);
      for (const track of stream.getTracks()) track.onended = () => this.stopMic();
    } catch (error) {
      stream?.getTracks().forEach(track => track.stop()); this.microphone = null;
      this.source?.disconnect(); this.lowpass?.disconnect(); this.source = this.lowpass = null;
      let message = this.errorText(error, 'Mikrofon');
      // Refusing microphone access must not also prevent ordinary reception.
      if (!this.running && !error.audioMessage) {
        try { await this.start(); }
        catch (outputError) { message += ` ${this.errorText(outputError, 'Wiedergabe')}`; }
      }
      this.error = message; throw error;
    } finally { this.micBusy = false; this.onChange(); }
  }
  stopMic() {
    this.micGeneration++;
    this.setCapture(null);
    this.source?.disconnect(); this.lowpass?.disconnect();
    this.source = null; this.lowpass = null;
    this.microphone?.getTracks().forEach(track => { track.onended = null; track.stop(); });
    this.microphone = null; this.onChange();
  }
  stop() {
    this.enabled = false; this.error = ''; this.warning = ''; this.stopMic(); this.reset();
    this.context?.suspend().catch(() => {}); this.onChange();
  }
  get running() { return this.enabled && this.context?.state === 'running'; }
  setGain(gain) { this.gain = gain; this.node?.port.postMessage({type: 'gain', value: gain}); }
  setCapture(identity) {
    if (identity === this.captureId) return;
    this.captureId = identity;
    this.node?.port.postMessage({type: 'capture', enabled: !!identity, generation: identity});
  }
  reset() { this.node?.port.postMessage({type: 'reset'}); }
  receive(buffer, state, stream, cutoff) {
    if (!this.running || buffer.byteLength !== 544) return;
    const v = new DataView(buffer);
    if (v.getUint32(0) !== 0x51545232 || v.getUint8(4) !== 1 || v.getUint8(5) !== 5 || v.getUint16(6) !== 64 || v.getUint32(60) !== state.session || v.getBigUint64(16) < cutoff || v.getUint32(40) !== 12000 || v.getUint16(52) !== 240 || v.getUint16(54) !== 2 || v.getUint32(56) !== 240) return;
    const desired = state.loop?.mode === 1 && stream === 1 ? 4 : stream;
    const epoch = desired === 4 ? state.tx?.epoch : state.rx?.[String(desired)]?.epoch;
    if (v.getUint32(8) !== desired || v.getUint32(12) !== epoch) return;
    const samples = new Float32Array(240);
    for (let i = 0; i < 240; i++) samples[i] = v.getInt16(64 + i * 2) / 32768;
    const identity = `${state.session}:${desired}:${epoch}`;
    this.node.port.postMessage({type: 'samples', identity, source: desired === 4 ? identity : `${state.session}:${desired}`, frequency: Number(v.getBigUint64(32)), index: Number(v.getBigUint64(24)), samples}, [samples.buffer]);
  }
}
