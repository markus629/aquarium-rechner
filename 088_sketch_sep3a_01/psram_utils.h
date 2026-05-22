#ifndef PSRAM_UTILS_H
#define PSRAM_UTILS_H

#include <Arduino.h>
#include <LittleFS.h>

// PSRAM-aware Array-Allokation für POD-Structs (Dosage, Measurement, etc.)
template<typename T>
T* psram_new_array(size_t count) {
  if (count == 0) return nullptr;
  void* ptr = psramFound() ? ps_calloc(count, sizeof(T)) : calloc(count, sizeof(T));
  return static_cast<T*>(ptr);
}

template<typename T>
void psram_delete_array(T* ptr) { free(ptr); }

// ============================================================================
// EFFIZIENTER DATENZUGRIFF: Sidecar-Offset + Tombstones
// ----------------------------------------------------------------------------
// Alle .bin-Dateien sind append-only und damit timestamp-aufsteigend sortiert.
// Statt bei jedem Delete/Cleanup die komplette Datei neu zu schreiben:
//   * Delete: Record mit timestamp=0 überschreiben (Tombstone, 12/28 Byte Write)
//   * Cleanup: Zähler in <file>.off erhöht; alter Prefix wird beim Lesen übersprungen
//   * Echte Compaction (Rewrite) nur wenn >50% der Datei übersprungen wird
// ============================================================================

// Records mit timestamp == 0 gelten als Tombstone (gelöscht).
// Echte Messwerte können nie timestamp 0 haben, weil savePHMeasurement /
// saveMeasurement nur schreiben wenn die Zeit initialisiert ist.
static inline bool isTombstoneTs(uint32_t ts) { return ts == 0; }

// Pfad der Sidecar-Offset-Datei zu <filename>.
// Hält einen uint32_t: Anzahl Records am Datei-Anfang, die beim Lesen
// übersprungen werden.
inline String offsetSidecarPath(const char* filename) {
  return String(filename) + ".off";
}

// Liest gespeicherten Prefix-Offset (in Records, nicht Bytes).
// 0 wenn keine Sidecar existiert.
inline uint32_t readPrefixOffset(const char* filename) {
  String sidecar = offsetSidecarPath(filename);
  if (!LittleFS.exists(sidecar)) return 0;
  File f = LittleFS.open(sidecar, FILE_READ);
  if (!f) return 0;
  uint32_t off = 0;
  f.read((uint8_t*)&off, sizeof(off));
  f.close();
  return off;
}

// Schreibt Prefix-Offset in Sidecar.
inline void writePrefixOffset(const char* filename, uint32_t offsetRecords) {
  String sidecar = offsetSidecarPath(filename);
  File f = LittleFS.open(sidecar, FILE_WRITE);
  if (!f) return;
  f.write((uint8_t*)&offsetRecords, sizeof(offsetRecords));
  f.close();
}

// Löscht Sidecar (z. B. nach echter Compaction).
inline void clearPrefixOffset(const char* filename) {
  String sidecar = offsetSidecarPath(filename);
  if (LittleFS.exists(sidecar)) LittleFS.remove(sidecar);
}

// --- Lese-Helfer ------------------------------------------------------------

// Liest alle gültigen Records (Tombstones gefiltert, Prefix übersprungen).
// Ergebnis ist bereits timestamp-aufsteigend sortiert, weil Daten append-only
// geschrieben werden und Delete-In-Place die Reihenfolge nicht ändert.
// Rückgabe: PSRAM-Array (mit psram_delete_array freigeben); nullptr bei leer.
template<typename T>
T* readValidEntries(const char* filename, int& count) {
  count = 0;
  if (!LittleFS.exists(filename)) return nullptr;

  File file = LittleFS.open(filename, FILE_READ);
  if (!file) return nullptr;

  size_t entrySize = sizeof(T);
  int totalRecords = file.size() / entrySize;
  uint32_t prefix = readPrefixOffset(filename);
  if ((int)prefix >= totalRecords) {
    file.close();
    return nullptr;
  }

  int rawCount = totalRecords - (int)prefix;
  T* raw = psram_new_array<T>(rawCount);
  if (!raw) {
    file.close();
    return nullptr;
  }

  file.seek(prefix * entrySize);
  size_t bytesRead = file.read((uint8_t*)raw, rawCount * entrySize);
  file.close();

  int readRecords = bytesRead / entrySize;
  // Tombstones kompaktieren (in-place).
  int out = 0;
  for (int i = 0; i < readRecords; i++) {
    if (!isTombstoneTs(raw[i].timestamp)) {
      if (out != i) raw[out] = raw[i];
      out++;
    }
  }

  count = out;
  if (out == 0) {
    psram_delete_array(raw);
    return nullptr;
  }
  return raw;
}

// Liefert Dateigröße in Records (ohne Prefix/Tombstone-Filter) — für Raw-Scans.
template<typename T>
int getTotalRecords(const char* filename) {
  if (!LittleFS.exists(filename)) return 0;
  File f = LittleFS.open(filename, FILE_READ);
  if (!f) return 0;
  int n = f.size() / sizeof(T);
  f.close();
  return n;
}

// Liest letzten gültigen (non-Tombstone) Record via Seek-from-End.
// O(1) im Normalfall, O(k) bei k aufeinanderfolgenden Tombstones am Ende.
template<typename T>
bool readLatestEntry(const char* filename, T& out) {
  if (!LittleFS.exists(filename)) return false;
  File f = LittleFS.open(filename, FILE_READ);
  if (!f) return false;
  int total = f.size() / sizeof(T);
  uint32_t prefix = readPrefixOffset(filename);
  for (int i = total - 1; i >= (int)prefix; i--) {
    f.seek((size_t)i * sizeof(T));
    T rec;
    if (f.read((uint8_t*)&rec, sizeof(T)) != sizeof(T)) break;
    if (!isTombstoneTs(rec.timestamp)) {
      out = rec;
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

// --- Delete per Tombstone ---------------------------------------------------

// Sucht Record mit gegebenem .index-Feld, überschreibt ihn mit Nullen
// (timestamp=0 = Tombstone). Rückgabe: true wenn gefunden.
// Vermeidet komplettes File-Rewrite.
template<typename T>
bool tombstoneByIndex(const char* filename, int targetIndex) {
  if (!LittleFS.exists(filename)) return false;
  File file = LittleFS.open(filename, "r+");
  if (!file) return false;

  size_t entrySize = sizeof(T);
  int total = file.size() / entrySize;
  uint32_t prefix = readPrefixOffset(filename);

  for (int i = (int)prefix; i < total; i++) {
    file.seek((size_t)i * entrySize);
    T rec;
    if (file.read((uint8_t*)&rec, entrySize) != entrySize) break;
    if (isTombstoneTs(rec.timestamp)) continue;
    if (rec.index == targetIndex) {
      T zero;
      memset(&zero, 0, sizeof(T));  // timestamp=0 → Tombstone
      file.seek((size_t)i * entrySize);
      file.write((uint8_t*)&zero, entrySize);
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

// --- Cleanup: Offset verschieben statt Rewrite ------------------------------

// Sucht ersten Record mit timestamp >= cutoffTime (ab aktuellem Prefix) und
// setzt den Prefix dorthin. Echter Rewrite nur wenn >50% der Datei
// übersprungen wird (Compaction, sonst wächst die Datei unbegrenzt).
template<typename T>
void cleanupFile(const char* filename, time_t cutoffTime) {
  if (!LittleFS.exists(filename)) return;

  File file = LittleFS.open(filename, FILE_READ);
  if (!file) return;

  size_t entrySize = sizeof(T);
  int total = file.size() / entrySize;
  if (total == 0) {
    file.close();
    return;
  }

  uint32_t prefix = readPrefixOffset(filename);
  if ((int)prefix >= total) {
    file.close();
    return;
  }

  // Binärsuche: erster Record (ab prefix) mit timestamp >= cutoffTime.
  // Daten sind aufsteigend; Tombstones haben timestamp=0 und werden immer
  // als "alt" behandelt (sie sollen weggeschoben werden).
  int lo = prefix;
  int hi = total;  // exklusiv
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    file.seek((size_t)mid * entrySize);
    T rec;
    if (file.read((uint8_t*)&rec, entrySize) != entrySize) {
      hi = mid;
      break;
    }
    uint32_t ts = rec.timestamp;
    if (isTombstoneTs(ts) || ts < (uint32_t)cutoffTime) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  file.close();

  uint32_t newPrefix = (uint32_t)lo;
  if (newPrefix <= prefix) {
    // Nichts zu tun.
    return;
  }

  // Compaction nur wenn der tote Prefix groß genug ist (>50% UND >=256 Records).
  // Sonst einfach Offset hochschreiben (4-Byte-Write).
  bool shouldCompact = (newPrefix * 2 > (uint32_t)total) && (newPrefix >= 256);

  if (!shouldCompact) {
    writePrefixOffset(filename, newPrefix);
    Serial.print("Cleanup (offset): ");
    Serial.print(filename);
    Serial.print(" prefix ");
    Serial.print(prefix);
    Serial.print(" -> ");
    Serial.println(newPrefix);
    return;
  }

  // --- Compaction: Datei neu schreiben ---
  File src = LittleFS.open(filename, FILE_READ);
  if (!src) return;
  const char* tmpName = "/_compact.tmp";
  if (LittleFS.exists(tmpName)) LittleFS.remove(tmpName);
  File dst = LittleFS.open(tmpName, FILE_WRITE);
  if (!dst) {
    src.close();
    return;
  }

  src.seek((size_t)newPrefix * entrySize);
  const size_t BUF = 512;
  uint8_t buf[BUF];
  while (src.available()) {
    size_t n = src.read(buf, BUF);
    if (n == 0) break;
    // Tombstones in diesem Durchlauf gleich mit entfernen: sie sind ohnehin
    // selten und wir wollen die Compaction nutzen, um sie mit aufzuräumen.
    for (size_t off = 0; off < n; off += entrySize) {
      if (off + entrySize > n) break;
      T* rec = reinterpret_cast<T*>(buf + off);
      if (!isTombstoneTs(rec->timestamp)) {
        dst.write((uint8_t*)rec, entrySize);
      }
    }
    yield();
  }
  src.close();
  dst.close();

  LittleFS.remove(filename);
  LittleFS.rename(tmpName, filename);
  clearPrefixOffset(filename);

  Serial.print("Cleanup (compact): ");
  Serial.print(filename);
  Serial.print(" prefix ");
  Serial.print(prefix);
  Serial.print(" removed, ");
  Serial.print(newPrefix);
  Serial.println(" old records purged");
}

// ============================================================================
// PsramPrint: Print-Subclass mit PSRAM-Puffer
// ----------------------------------------------------------------------------
// serializeJson(doc, psramPrint) schreibt direkt in einen ps_realloc-Puffer,
// ohne Zwischen-String im Heap. Vermeidet den ~500 KB Heap-Peak, der sonst
// beim Serialisieren großer PsramJsonDocuments entsteht.
// ============================================================================
class PsramPrint : public Print {
public:
  explicit PsramPrint(size_t initialCap = 4096) : _cap(0), _len(0), _buf(nullptr) {
    reserve(initialCap);
  }
  ~PsramPrint() {
    if (_buf) free(_buf);
  }

  // Non-copyable: besitzt PSRAM-Puffer.
  PsramPrint(const PsramPrint&) = delete;
  PsramPrint& operator=(const PsramPrint&) = delete;

  bool reserve(size_t needed) {
    if (needed <= _cap) return true;
    size_t newCap = _cap ? _cap : 4096;
    while (newCap < needed) newCap *= 2;
    void* p = psramFound() ? ps_realloc(_buf, newCap) : realloc(_buf, newCap);
    if (!p) return false;
    _buf = static_cast<char*>(p);
    _cap = newCap;
    return true;
  }

  size_t write(uint8_t c) override {
    if (!reserve(_len + 1)) return 0;
    _buf[_len++] = static_cast<char>(c);
    return 1;
  }

  size_t write(const uint8_t* data, size_t size) override {
    if (!size) return 0;
    if (!reserve(_len + size)) return 0;
    memcpy(_buf + _len, data, size);
    _len += size;
    return size;
  }

  const char* data() const { return _buf ? _buf : ""; }
  size_t size() const { return _len; }
  void clear() { _len = 0; }

private:
  size_t _cap;
  size_t _len;
  char*  _buf;
};

#endif
