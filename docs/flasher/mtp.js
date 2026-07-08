// mtp.js — minimal MTP-over-WebUSB, forked to add a write path.
//
// The transaction core (container build/parse, bulk read/write, OpenSession,
// GetObjectHandles, GetObject) is derived from tidepool-org/webmtp
// (https://github.com/tidepool-org/webmtp, BSD-2-Clause). That library is
// read-only; the additions here — GetStorageIDs, GetObjectInfo, SendObjectInfo,
// SendObject and the GARMIN/Apps navigation — are what let us *upload* a
// Connect IQ .prg to a watch, the browser equivalent of the repo's
// tools/mtp_send_to_folder.c.
//
// This only works where the OS doesn't already own the MTP interface: macOS
// (no kernel MTP driver) is the happy path, most Linux works once the device
// is unmounted from gvfs, and Windows generally won't let the browser claim it.

const CODE = {
  GET_STORAGE_IDS:       { value: 0x1004, name: 'GetStorageIDs' },
  OPEN_SESSION:          { value: 0x1002, name: 'OpenSession' },
  CLOSE_SESSION:         { value: 0x1003, name: 'CloseSession' },
  GET_OBJECT_HANDLES:    { value: 0x1007, name: 'GetObjectHandles' },
  GET_OBJECT_INFO:       { value: 0x1008, name: 'GetObjectInfo' },
  GET_OBJECT:            { value: 0x1009, name: 'GetObject' },
  SEND_OBJECT_INFO:      { value: 0x100C, name: 'SendObjectInfo' },
  SEND_OBJECT:           { value: 0x100D, name: 'SendObject' },
  OK:                    { value: 0x2001, name: 'OK' },
};

const TYPE = ['undefined', 'Command Block', 'Data Block', 'Response Block', 'Event Block'];

const FMT_ASSOCIATION = 0x3001; // a folder
const FMT_UNDEFINED   = 0x3000; // a plain file
const ROOT            = 0xFFFFFFFF;

export default class Mtp extends EventTarget {
  constructor(device) {
    super();
    this.state = 'open';
    this.transactionID = 0;
    this.device = device;

    (async () => {
      if (this.device.opened) await this.device.close();
      await this.device.open();
      await this.device.selectConfiguration(1);

      // Find the interface that actually carries bulk IN + OUT endpoints,
      // rather than assuming interface 0 (Garmin's layout varies by model).
      let chosen = null;
      for (const iface of this.device.configuration.interfaces) {
        const alt = iface.alternate;
        const epOut = alt.endpoints.find((e) => e.direction === 'out' && e.type === 'bulk');
        const epIn  = alt.endpoints.find((e) => e.direction === 'in'  && e.type === 'bulk');
        if (epOut && epIn) { chosen = { iface, epOut, epIn }; break; }
      }
      if (!chosen) throw new Error('No MTP bulk interface found on this device.');

      await this.device.claimInterface(chosen.iface.interfaceNumber);
      this.usb = {
        interfaceNumber: chosen.iface.interfaceNumber,
        outEPnum: chosen.epOut.endpointNumber,
        inEPnum:  chosen.epIn.endpointNumber,
        inPacketSize: chosen.epIn.packetSize || 512,
      };
      this.dispatchEvent(new Event('ready'));
    })().catch((error) => {
      this.lastError = error;
      this.dispatchEvent(new Event('error'));
    });
  }

  getName(list, idx) {
    for (const k in list) if (list[k].value === idx) return list[k].name;
    return 'unknown';
  }

  // --- container plumbing ---------------------------------------------------

  buildCommand(code, payload = []) {
    const len = 12 + payload.length * 4;
    const dv = new DataView(new ArrayBuffer(len));
    dv.setUint32(0, len, true);
    dv.setUint16(4, 1, true);        // Command Block
    dv.setUint16(6, code, true);
    dv.setUint32(8, this.transactionID, true);
    payload.forEach((p, i) => dv.setUint32(12 + i * 4, p >>> 0, true));
    this.transactionID += 1;
    return dv.buffer;
  }

  // Data block that reuses the transaction id of the command it follows.
  buildData(code, transactionID, bytes) {
    const len = 12 + bytes.byteLength;
    const buf = new ArrayBuffer(len);
    const dv = new DataView(buf);
    dv.setUint32(0, len, true);
    dv.setUint16(4, 2, true);        // Data Block
    dv.setUint16(6, code, true);
    dv.setUint32(8, transactionID, true);
    new Uint8Array(buf, 12).set(new Uint8Array(bytes));
    return buf;
  }

  parse(bytes, length) {
    const fields = {
      type: TYPE[bytes.getUint16(4, true)],
      code: this.getName(CODE, bytes.getUint16(6, true)),
      rawCode: bytes.getUint16(6, true),
      transactionID: bytes.getUint32(8, true),
      payload: bytes.buffer.slice(12, length),
      parameters: [],
    };
    for (let i = 12; i + 4 <= length; i += 4) fields.parameters.push(bytes.getUint32(i, true));
    return fields;
  }

  async write(buffer) {
    return this.device.transferOut(this.usb.outEPnum, buffer);
  }

  async read() {
    let result = await this.device.transferIn(this.usb.inEPnum, this.usb.inPacketSize);
    if (!(result && result.data && result.data.byteLength)) return result;

    let raw = new Uint8Array(result.data.buffer);
    const containerLength = new DataView(result.data.buffer).getUint32(0, true);
    while (raw.byteLength < containerLength) {
      result = await this.device.transferIn(this.usb.inEPnum, this.usb.inPacketSize);
      const merged = new Uint8Array(raw.byteLength + result.data.byteLength);
      merged.set(raw);
      merged.set(new Uint8Array(result.data.buffer), raw.byteLength);
      raw = merged;
    }
    return this.parse(new DataView(raw.buffer), containerLength);
  }

  async readData() {
    let r = null;
    do { r = await this.read(); if (!r) throw new Error('No data returned from device'); }
    while (r.type !== 'Data Block');
    return r;
  }

  // A command that expects Data-in then a Response. Consumes both so the next
  // transaction starts clean, and checks the response code.
  async dataIn(code, payload = []) {
    await this.write(this.buildCommand(code, payload));
    const data = await this.readData();
    const resp = await this.read();
    if (resp && resp.code !== 'OK') throw new Error(`${this.getName(CODE, code)} failed: ${resp.code}`);
    return data;
  }

  // --- MTP strings ----------------------------------------------------------

  mtpString(str) {
    if (!str) return new Uint8Array([0]);
    const buf = new ArrayBuffer(1 + (str.length + 1) * 2);
    const dv = new DataView(buf);
    dv.setUint8(0, str.length + 1); // char count incl. null terminator
    for (let i = 0; i < str.length; i++) dv.setUint16(1 + i * 2, str.charCodeAt(i), true);
    return new Uint8Array(buf);
  }

  readMtpString(dv, offset) {
    const count = dv.getUint8(offset);
    let s = '';
    for (let i = 0; i < count - 1; i++) s += String.fromCharCode(dv.getUint16(offset + 1 + i * 2, true));
    return { value: s, next: offset + 1 + count * 2 };
  }

  // --- operations -----------------------------------------------------------

  async openSession() {
    await this.write(this.buildCommand(CODE.OPEN_SESSION.value, [1]));
    const resp = await this.read();
    // 0x2019 = Session Already Open — fine to proceed.
    if (resp && resp.code !== 'OK' && resp.rawCode !== 0x2019) throw new Error(`OpenSession failed: ${resp.code}`);
  }

  async close() {
    try {
      await this.write(this.buildCommand(CODE.CLOSE_SESSION.value, [1]));
      await this.read();
    } catch (_) { /* ignore */ }
    try { await this.device.releaseInterface(this.usb.interfaceNumber); } catch (_) {}
    try { await this.device.close(); } catch (_) {}
  }

  async getStorageIDs() {
    const data = await this.dataIn(CODE.GET_STORAGE_IDS.value);
    const dv = new DataView(data.payload);
    const count = dv.getUint32(0, true);
    const ids = [];
    for (let i = 0; i < count; i++) ids.push(dv.getUint32(4 + i * 4, true));
    return ids;
  }

  async getObjectHandles(storageId, parent = ROOT, format = 0) {
    const data = await this.dataIn(CODE.GET_OBJECT_HANDLES.value, [storageId, format, parent]);
    const dv = new DataView(data.payload);
    const count = dv.getUint32(0, true);
    const handles = [];
    for (let i = 0; i < count; i++) handles.push(dv.getUint32(4 + i * 4, true));
    return handles;
  }

  async getObjectInfo(handle) {
    const data = await this.dataIn(CODE.GET_OBJECT_INFO.value, [handle]);
    const dv = new DataView(data.payload);
    return {
      storageId: dv.getUint32(0, true),
      format: dv.getUint16(4, true),
      filename: this.readMtpString(dv, 52).value,
    };
  }

  // Reserve an object in a folder (Data-out), returning its new handle.
  async sendObjectInfo(storageId, parentHandle, filename, size) {
    const fn = this.mtpString(filename);
    const dataset = new ArrayBuffer(52 + fn.byteLength + 3); // + 3 empty date/keyword strings
    const dv = new DataView(dataset);
    dv.setUint32(0, storageId, true);
    dv.setUint16(4, FMT_UNDEFINED, true);     // ObjectFormat
    dv.setUint16(6, 0, true);                 // ProtectionStatus
    dv.setUint32(8, size >>> 0, true);        // ObjectCompressedSize
    // thumb/image fields (12..37) stay zero
    dv.setUint32(38, 0, true);                // ParentObject (0 → use command param)
    dv.setUint16(42, 0, true);                // AssociationType (0 = file)
    dv.setUint32(44, 0, true);                // AssociationDesc
    dv.setUint32(48, 0, true);                // SequenceNumber
    new Uint8Array(dataset, 52).set(fn);      // Filename, then empty dates/keywords (already 0)

    const txid = this.transactionID;
    await this.write(this.buildCommand(CODE.SEND_OBJECT_INFO.value, [storageId, parentHandle]));
    await this.write(this.buildData(CODE.SEND_OBJECT_INFO.value, txid, dataset));
    const resp = await this.read();
    if (!resp || resp.code !== 'OK') throw new Error(`SendObjectInfo rejected: ${resp ? resp.code : 'no response'}`);
    return resp.parameters[2]; // [storageID, parentHandle, newObjectHandle]
  }

  async sendObject(arrayBuffer) {
    const txid = this.transactionID;
    await this.write(this.buildCommand(CODE.SEND_OBJECT.value));
    await this.write(this.buildData(CODE.SEND_OBJECT.value, txid, arrayBuffer));
    const resp = await this.read();
    if (!resp || resp.code !== 'OK') throw new Error(`SendObject rejected: ${resp ? resp.code : 'no response'}`);
  }

  // Walk a path of association (folder) names from the storage root.
  async findFolder(storageId, names) {
    let parent = ROOT;
    let handle = null;
    for (const target of names) {
      handle = null;
      for (const h of await this.getObjectHandles(storageId, parent)) {
        const info = await this.getObjectInfo(h);
        if (info.format === FMT_ASSOCIATION && info.filename.toLowerCase() === target.toLowerCase()) {
          handle = h;
          break;
        }
      }
      if (handle === null) throw new Error(`Folder "${target}" not found`);
      parent = handle;
    }
    return handle;
  }

  // Locate GARMIN/Apps across whichever storage holds it.
  async findGarminApps() {
    let lastErr;
    for (const storageId of await this.getStorageIDs()) {
      try {
        return { storageId, handle: await this.findFolder(storageId, ['GARMIN', 'Apps']) };
      } catch (e) { lastErr = e; }
    }
    throw new Error(`Couldn't find GARMIN/Apps on the device (${lastErr ? lastErr.message : 'no storage'})`);
  }
}

const GARMIN_VENDOR_ID = 0x091e;

// Must be called from a user gesture (a click) — this triggers the browser's
// device-picker. Keep it as the first await in the handler so the gesture
// isn't consumed first.
export function requestGarminDevice() {
  if (!navigator.usb) throw new Error('This browser has no WebUSB. Use desktop Chrome or Edge.');
  return navigator.usb.requestDevice({ filters: [{ vendorId: GARMIN_VENDOR_ID }] });
}

// Upload a Connect IQ .prg into GARMIN/Apps on an already-chosen device.
export async function installPrg(device, file, onLog = () => {}) {
  const buf = await file.arrayBuffer();

  const mtp = await new Promise((resolve, reject) => {
    const m = new Mtp(device);
    m.addEventListener('ready', () => resolve(m));
    m.addEventListener('error', () => reject(m.lastError || new Error('Could not open the device over USB.')));
  });

  try {
    onLog('Opening MTP session…');
    await mtp.openSession();

    onLog('Locating GARMIN/Apps…');
    const { storageId, handle } = await mtp.findGarminApps();

    onLog(`Sending ${file.name} (${buf.byteLength.toLocaleString()} bytes)…`);
    await mtp.sendObjectInfo(storageId, handle, file.name, buf.byteLength);
    await mtp.sendObject(buf);

    onLog('Installed. Unplug the watch to load it.');
  } finally {
    await mtp.close();
  }
}
