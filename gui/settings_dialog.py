"""Hardware settings editor; authoritative values and storage live in the backend."""
from decimal import Decimal
import tkinter as tk


def show_settings(root, receiver, stop_audio_tx):
    existing=root.children.get('settings_dialog')
    if existing is not None:
        existing.lift();return
    client=receiver.settings
    if client.values is None: return
    window=tk.Toplevel(root,name='settings_dialog')
    window.title('QO100_TRX2 — Einstellungen')
    window.resizable(False,False)
    variables={};entries={};ticket=None;poll_timer=None
    source=client.values.copy()
    connection=tk.StringVar(value=source['pluto_connection'])
    tk.Label(window,text='Pluto-Verbindung').grid(row=0,column=0,sticky='w',padx=12,pady=8)
    modes=tk.Frame(window);modes.grid(row=0,column=1,sticky='w')
    for text,value in (('Netzwerk','network'),('USB','usb')):
        tk.Radiobutton(modes,text=text,variable=connection,value=value).pack(side='left')
    fields=(('pluto_ip','Pluto IPv4-Adresse','text'),
            ('lnb_reference_hz','LNB-Referenz (MHz), Faktor 390','mhz'),
            ('rx_correction_hz','Pluto RX-Korrektur (Hz)','int'),
            ('tx_correction_hz','Pluto TX-Korrektur (Hz)','int'),
            ('pluto_tx_base_hz','Pluto TX-Basisfrequenz (MHz)','mhz'),
            ('tx_attenuation_db','TX-Dämpfung (0–40 dB, Schritt 0,25)','float'),
            ('gui_wheel_step_hz','Mausrad-Schrittweite (Hz)','int'))
    for row,(key,label,kind) in enumerate(fields,1):
        text=source[key]
        if kind=='mhz': text=f'{int(text)/1e6:.6f}'
        variable=tk.StringVar(value=text);variables[key]=variable
        tk.Label(window,text=label).grid(row=row,column=0,sticky='w',padx=12,pady=5)
        entry=tk.Entry(window,name=key,textvariable=variable,width=23)
        entry.grid(row=row,column=1,sticky='ew',padx=12,pady=5);entries[key]=entry
    computed=tk.Label(window,justify='left',anchor='w')
    computed.grid(row=8,column=0,columnspan=2,sticky='w',padx=12,pady=8)
    status=tk.Label(window,text='Werte vom Backend geladen',justify='left',wraplength=500,anchor='w')
    status.grid(row=9,column=0,columnspan=2,sticky='w',padx=12,pady=5)
    buttons=tk.Frame(window);buttons.grid(row=10,column=0,columnspan=2,pady=12)
    def values():
        patch={'pluto_connection':connection.get()}
        for key,_,kind in fields:
            text=variables[key].get().strip().replace(',','.')
            if kind=='text': patch[key]=text;continue
            number=Decimal(text)*(1000000 if kind=='mhz' else 1)
            if not number.is_finite() or (kind!='float' and number!=number.to_integral_value()):
                raise ValueError('Frequenzen und Korrekturen müssen auf ganze Hz auflösbar sein')
            patch[key]=str(number if kind=='float' else int(number))
        return patch
    def preview(*args):
        entries['pluto_ip'].configure(state='normal' if connection.get()=='network' else 'disabled')
        try:
            v=values()
            rx=int(source['rx_center_hz'])-int(v['lnb_reference_hz'])*390+int(v['rx_correction_hz'])
            tx=int(v['pluto_tx_base_hz'])+int(v['tx_correction_hz'])
            computed.configure(text=f'Berechneter Pluto RX: {rx/1e6:.6f} MHz\nBerechneter Pluto TX: {tx/1e6:.6f} MHz\nPositive Korrektur erhöht die jeweilige Pluto-Frequenz.')
        except (ValueError,ArithmeticError): computed.configure(text='Berechnete Frequenzen: Eingaben prüfen')
    for variable in (*variables.values(),connection): variable.trace_add('write',preview)
    def apply():
        nonlocal ticket
        try:
            patch={k:v for k,v in values().items() if v!=source.get(k)}
            if not patch: status.configure(text='Keine Änderungen.');return
            stop_audio_tx()
            ticket=client.set(patch)
            apply_button.configure(state='disabled')
            status.configure(text='Wird geprüft, übernommen und gespeichert …')
        except (ValueError,ArithmeticError,RuntimeError) as exc: status.configure(text=str(exc))
    apply_button=tk.Button(buttons,name='apply_settings',text='Übernehmen und speichern',command=apply)
    apply_button.pack(side='left',padx=8)
    tk.Button(buttons,text='Schließen',command=window.destroy).pack(side='left',padx=8)
    def poll():
        nonlocal ticket,source,poll_timer
        if not window.winfo_exists(): return
        if ticket is not None and ticket in client.results:
            code,error=client.results[ticket];ticket=None
            status.configure(text=('Gespeichert und aktiv.' if code==0 else 'Nicht übernommen: '+(error or f'Status {code}')))
            if code==0: source=client.values.copy()
        apply_button.configure(state='disabled' if ticket is not None or client.pending else 'normal')
        poll_timer=window.after(100,poll)
    def destroyed(event):
        if event.widget is window and poll_timer is not None: window.after_cancel(poll_timer)
    window.bind('<Destroy>',destroyed)
    preview();poll();window.transient(root)
