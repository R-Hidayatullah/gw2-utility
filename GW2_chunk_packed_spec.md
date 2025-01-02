# Guild Wars 2 `.dat` Packed File Format (Decompressed)

This document provides a detailed explanation of the structure of packed files inside the Guild Wars 2 `.dat` file after decompression. The packed files contain multiple chunks, each with metadata and data sections, organized as described below.

---

## **File Structure**

### **1. Main Header**

The main header describes the basic metadata for the packed file.

| Field          | Type       | Description                                                                 |
|-----------------|------------|-----------------------------------------------------------------------------|
| `magic`        | `char[2]`  | Magic number identifying the file type.                                     |
| `version`      | `uint16`   | Version of the packed file format.                                          |
| `zero`         | `uint16`   | Reserved, always zero.                                                      |
| `headerSize`   | `uint16`   | Size of the header in bytes.                                                |
| `type`         | `char[4]`  | Identifier for the type of packed file.                                     |

---

### **2. Chunk Structure**

The packed file consists of multiple chunks. Each chunk contains a header and data, with optional offset tables.

#### **ChunkHeader**

The `ChunkHeader` provides metadata about a specific chunk.

| Field                   | Type       | Description                                                                 |
|--------------------------|------------|-----------------------------------------------------------------------------|
| `magic`                 | `char[4]`  | Magic number identifying the chunk type.                                   |
| `chunkSize`             | `uint32`   | Total size of the chunk in bytes.                                          |
| `version`               | `uint16`   | Version of the chunk format.                                               |
| `headerSize`            | `uint16`   | Size of the chunk header in bytes.                                         |
| `offsetToOffsetTable`   | `uint32`   | Offset to the offset table within the chunk. If `0`, no offset table exists.|

#### **ChunkData**

The `ChunkData` structure contains the actual data for the chunk. Its organization depends on the presence of an offset table.

| Field                | Type                  | Description                                                                 |
|-----------------------|-----------------------|-----------------------------------------------------------------------------|
| `chunkHeader`        | `ChunkHeader`         | Header metadata for the chunk.                                             |
| `data`               | `ubyte[]`            | Raw data in the chunk.                                                     |
| `offsetTable`        | `uint32[]` (optional)| Array of offsets within the chunk. Present only if `offsetToOffsetTable != 0`. |
| `unknown`            | `ubyte[]` (optional) | Padding or additional unknown data, calculated as:                         |
|                       |                       | `chunkSize - 8 - offsetToOffsetTable - 4 - (nbOfOffsets * 4)`.             |

---

## **Reading the Packed File**

1. **Parse the Main Header:**
   - Read the `headerDat` structure to extract file-level metadata.

2. **Iterate Through Chunks:**
   - For each chunk, read the `ChunkHeader` to determine its size and structure.

3. **Handle Offset Tables:**
   - If `offsetToOffsetTable != 0`, read the offset table and use it to navigate the chunk data.
   - Otherwise, treat the entire chunk as raw data.

4. **Process Data:**
   - Use the `offsetTable` (if present) to interpret the data organization within the chunk.
   - Handle any unknown data or padding appropriately.

---

## **Notes**

- **Chunk Size Calculation:**
  The `chunkSize` field in the `ChunkHeader` includes the header, data, and any padding or offset tables.

- **Offset Table:**
  The offset table allows for more complex data organization within a chunk. Its size is determined by the `nbOfOffsets` field.

- **Unknown Fields:**
  The `unknown` field represents unused or unidentified data and may vary in size depending on the chunk's structure.

- **End of File:**
  The file terminates when no additional chunks are present.

---

This documentation provides the foundational structure for interpreting decompressed packed files in Guild Wars 2 `.dat` files. Further analysis may be required to fully understand the unknown fields and data within specific chunk types.