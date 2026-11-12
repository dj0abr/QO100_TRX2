import {text as t} from './language.js';

const preference = 'qo100.sstvOverlay';
const freeTextLayer = 3;
const fonts = ['sans-serif', 'serif', 'monospace'];
const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
const arrows = {ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1]};

function loadOverlay() {
  let saved;
  try { saved = JSON.parse(localStorage.getItem(preference)); } catch {}
  const defaults = [
    {x: 12, y: 12, size: 28, bold: true},
    {x: 12, y: 80, size: 24, bold: true},
    {x: 12, y: 50, size: 18, bold: true},
    {x: 12, y: 218, size: 24, bold: true},
  ];
  const savedLayers = Array.isArray(saved?.layers) ? saved.layers : [];
  const texts = Array.isArray(saved?.texts) ? saved.texts : [];
  if (savedLayers.length === 3) {
    // Insert the new callsign without shifting the meaning of saved fields.
    savedLayers.splice(1, 0, null);
    texts.splice(1, 0, '');
  }
  const layers = defaults.map((layer, index) => {
    const item = savedLayers[index];
    return {
      x: Number.isFinite(item?.x) ? item.x : layer.x,
      y: Number.isFinite(item?.y) ? item.y : layer.y,
      size: Number.isFinite(item?.size) ? clamp(Math.round(item.size), 10, 48) : layer.size,
      bold: typeof item?.bold === 'boolean' ? item.bold : layer.bold,
      font: fonts.includes(item?.font) ? item.font : fonts[0],
    };
  });
  const slot = ['1', '2', '3', '4', '5'].includes(saved?.slot) ? saved.slot : '1';
  return {layers, texts, slot};
}

export class SstvTxView {
  constructor(panel, dialog, imageChanged) {
    const tx = id => panel.querySelector(`#sstv-tx-${id}`);
    const editor = id => dialog.querySelector(`#sstv-editor-${id}`);
    this.image = tx('image');
    this.context = this.image.getContext('2d');
    this.file = tx('file');
    this.edit = tx('edit');
    this.slot = tx('text-slot');
    this.dialog = dialog;
    this.preview = editor('preview');
    this.previewContext = this.preview.getContext('2d');
    this.layer = editor('layer');
    this.font = editor('font');
    this.size = editor('size');
    this.bold = editor('bold');
    this.fields = [...dialog.querySelectorAll('[data-sstv-text]')];
    this.handles = [...dialog.querySelectorAll('.sstv-editor-handle')];
    this.boxes = [];
    this.load = 0;
    this.base = document.createElement('canvas');
    this.base.width = this.image.width;
    this.base.height = this.image.height;

    const {layers, texts, slot} = loadOverlay();
    this.layers = layers;
    this.slot.value = slot;
    this.slot.addEventListener('change', () => { this.render(); this.save(); });
    this.fields.forEach((field, index) => {
      if (typeof texts?.[index] === 'string') field.value = texts[index].slice(0, field.maxLength);
      const selectText = () => {
        if (index >= freeTextLayer) this.slot.value = String(index - freeTextLayer + 1);
        this.selectLayer(Math.min(index, freeTextLayer));
        this.render();
      };
      field.addEventListener('focus', selectText);
      field.addEventListener('input', selectText);
    });
    this.layer.addEventListener('change', () => this.selectLayer(Number(this.layer.value)));
    for (const field of [this.font, this.size, this.bold]) {
      field.addEventListener('change', () => {
        const layer = this.layers[Number(this.layer.value)];
        layer.font = this.font.value;
        layer.bold = this.bold.checked;
        if (Number.isFinite(this.size.valueAsNumber)) layer.size = clamp(Math.round(this.size.valueAsNumber), 10, 48);
        this.size.value = layer.size;
        this.render();
      });
    }
    this.handles.forEach((handle, index) => this.bindHandle(handle, index));
    this.edit.addEventListener('click', () => {
      dialog.showModal();
      this.selectLayer(Number(this.layer.value));
      this.render();
    });
    dialog.addEventListener('close', () => { this.drag = null; this.save(); });

    const drop = tx('drop'), hint = tx('hint'), error = tx('error');
    const loadFile = async file => {
      const load = ++this.load;
      error.textContent = '';
      const url = URL.createObjectURL(file);
      try {
        const image = new Image();
        image.src = url;
        await image.decode();
        if (load !== this.load) return;

        const {width, height} = this.base;
        const scale = Math.min(width / image.naturalWidth, height / image.naturalHeight);
        const w = image.naturalWidth * scale, h = image.naturalHeight * scale;
        const context = this.base.getContext('2d');
        context.fillStyle = '#000';
        context.fillRect(0, 0, width, height);
        context.drawImage(image, (width - w) / 2, (height - h) / 2, w, h);
        this.image.hidden = false;
        hint.hidden = true;
        this.render();
        imageChanged();
      } catch {
        if (load === this.load) error.textContent = t('Bild konnte nicht geöffnet werden.', 'Could not open the image.');
      } finally {
        URL.revokeObjectURL(url);
      }
    };
    drop.addEventListener('dragover', event => {
      event.preventDefault();
      const files = !this.file.disabled && event.dataTransfer.types.includes('Files');
      event.dataTransfer.dropEffect = files ? 'copy' : 'none';
      drop.toggleAttribute('data-dragging', files);
    });
    drop.addEventListener('dragleave', () => drop.removeAttribute('data-dragging'));
    drop.addEventListener('drop', event => {
      event.preventDefault();
      drop.removeAttribute('data-dragging');
      if (this.file.disabled) return;
      const files = event.dataTransfer.files;
      if (files.length === 1) loadFile(files[0]);
      else error.textContent = t('Bitte eine Bilddatei ablegen.', 'Drop one image file.');
    });
    this.file.addEventListener('change', () => {
      if (this.file.files.length) loadFile(this.file.files[0]);
      this.file.value = '';
    });
  }

  setBusy(busy) {
    this.file.disabled = busy;
    this.edit.disabled = this.slot.disabled = busy || this.image.hidden;
  }

  save() {
    const texts = this.fields.map(field => field.value);
    try { localStorage.setItem(preference, JSON.stringify({texts, layers: this.layers, slot: this.slot.value})); } catch {}
  }

  selectLayer(index) {
    this.layer.value = index;
    const layer = this.layers[index];
    this.font.value = layer.font;
    this.size.value = layer.size;
    this.bold.checked = layer.bold;
    this.handles.forEach((handle, i) => handle.toggleAttribute('data-selected', i === index));
  }

  bindHandle(handle, index) {
    handle.addEventListener('focus', () => this.selectLayer(index));
    handle.addEventListener('pointerdown', event => {
      if (event.button !== 0 || this.drag) return;
      this.selectLayer(index);
      const {x, y} = this.boxes[index];
      this.drag = {id: event.pointerId, clientX: event.clientX, clientY: event.clientY, x, y};
      handle.setPointerCapture(event.pointerId);
    });
    handle.addEventListener('pointermove', event => {
      const drag = this.drag;
      if (drag?.id !== event.pointerId) return;
      const rect = this.preview.getBoundingClientRect();
      const dx = (event.clientX - drag.clientX) * this.image.width / rect.width;
      const dy = (event.clientY - drag.clientY) * this.image.height / rect.height;
      this.moveLayer(index, drag.x + dx, drag.y + dy);
    });
    handle.addEventListener('lostpointercapture', () => { this.drag = null; });
    handle.addEventListener('keydown', event => {
      if (!Object.hasOwn(arrows, event.key)) return;
      event.preventDefault();
      const [dx, dy] = arrows[event.key], step = event.shiftKey ? 5 : 1;
      const {x, y} = this.boxes[index];
      this.moveLayer(index, x + dx * step, y + dy * step);
    });
  }

  moveLayer(index, x, y) {
    const box = this.boxes[index], layer = this.layers[index];
    layer.x = clamp(Math.round(x), 0, this.image.width - box.width);
    layer.y = clamp(Math.round(y), 0, this.image.height - box.height);
    this.render();
  }

  render() {
    if (this.image.hidden) return;
    const {width, height} = this.image, context = this.context;
    context.drawImage(this.base, 0, 0);
    context.textBaseline = 'middle';
    context.lineWidth = 3;
    context.lineJoin = 'round';
    context.strokeStyle = '#000';
    context.fillStyle = '#fff';

    const activeField = freeTextLayer + Number(this.slot.value) - 1;
    this.layers.forEach((layer, index) => {
      const field = this.fields[index === freeTextLayer ? activeField : index];
      const text = field.value.trim(), handle = this.handles[index];
      handle.hidden = !text;
      if (!text) return;
      context.font = `${layer.bold ? 'bold ' : ''}${layer.size}px ${layer.font}`;
      const x = clamp(layer.x, 0, width - 5);
      const w = Math.min(width - x, Math.ceil(context.measureText(text).width) + 4);
      const h = Math.ceil(layer.size * 1.25) + 4;
      const y = clamp(layer.y, 0, height - h);
      context.strokeText(text, x + 2, y + h / 2, w - 4);
      context.fillText(text, x + 2, y + h / 2, w - 4);
      this.boxes[index] = {x, y, width: w, height: h};
      if (this.dialog.open) {
        handle.style.left = `${100 * x / width}%`;
        handle.style.top = `${100 * y / height}%`;
        handle.style.width = `${100 * w / width}%`;
        handle.style.height = `${100 * h / height}%`;
      }
    });
    if (this.dialog.open) {
      this.fields.forEach((field, index) => field.toggleAttribute('data-preview', index === activeField));
      this.previewContext.drawImage(this.image, 0, 0);
    }
  }
}
