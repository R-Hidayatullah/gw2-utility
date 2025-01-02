# Guild Wars 2 `.dat` File Format Documentation

This document provides a detailed explanation of the structure and organization of the Guild Wars 2 `.dat` file based on the provided data structures and logic.

---

## **Overview**

The `.dat` file is a container format used by Guild Wars 2 to store game assets such as textures, models, and other resources. It is organized into a header, metadata, and data chunks. Each chunk may be compressed and contains CRC32C checksums for integrity verification.

---

## **File Structure**

### **1. DatHeader**
The `DatHeader` structure defines the primary header of the `.dat` file.

| Field             | Type    | Description                                                                 |
|--------------------|---------|-----------------------------------------------------------------------------|
| `version`         | `uint8` | File format version.                                                        |
| `identifier`      | `char[3]` | Identifier for the file type.                                              |
| `header_size`     | `uint32` | Size of the header in bytes.                                                |
| `unknown_field`   | `uint32` | Unknown purpose.                                                            |
| `chunk_size`      | `uint32` | Total size of the chunk containing the header.                              |
| `crc`             | `uint32` | CRC32C checksum of the header.                                              |
| `unknown_field_2` | `uint32` | Unknown purpose.                                                            |
| `mft_offset`      | `uint64` | Offset to the Master File Table (MFT).                                      |
| `mft_size`        | `uint32` | Size of the MFT in bytes.                                                   |
| `flag`            | `uint32` | Additional metadata flags.                                                  |

**Notes:**
- Padding is added to align the header chunk to `chunk_size - header_size`.

---

### **2. Master File Table (MFT)**

#### **MftHeader**
The `MftHeader` structure describes the MFT.

| Field             | Type    | Description                                                                 |
|--------------------|---------|-----------------------------------------------------------------------------|
| `identifier`      | `char[4]` | Identifier for the MFT section.                                            |
| `unknown_field`   | `uint64` | Unknown purpose.                                                            |
| `mft_entry_size`  | `uint32` | Size of each entry in the MFT.                                              |
| `unknown_field_2` | `uint32` | Unknown purpose.                                                            |
| `unknown_field_3` | `uint32` | Unknown purpose.                                                            |

#### **MftData**
The `MftData` structure describes individual entries in the MFT.

| Field             | Type    | Description                                                                 |
|--------------------|---------|-----------------------------------------------------------------------------|
| `offset`          | `uint64` | Offset to the data chunk.                                                   |
| `size`            | `uint32` | Size of the data chunk in bytes.                                            |
| `compression_flag`| `uint16` | Indicates whether the chunk is compressed.                                  |
| `entry_flag`      | `uint16` | Flags for the entry (e.g., type or permissions).                            |
| `counter`         | `uint32` | Unknown purpose.                                                            |
| `crc`             | `uint32` | CRC32C checksum of the data chunk.                                          |

#### **MftIndexData**
The `MftIndexData` structure maps file IDs to base IDs.

| Field             | Type    | Description                                                                 |
|--------------------|---------|-----------------------------------------------------------------------------|
| `file_id`         | `uint32` | Unique identifier for the file.                                             |
| `base_id`         | `uint32` | Identifier for the base entry (e.g., parent or related data).                |

---

## **Data Chunk Organization**

### **Compression**
- Each data chunk may be compressed. The `compression_flag` in the `MftData` structure indicates the compression status.

### **CRC32C Checksums**
- Data chunks include CRC32C checksums:
  - Every `0x10000 - 4` bytes contains a checksum.
  - An additional checksum is located 4 bytes before the end of the file.

### **Reading Data**
1. Use the `offset` field in `MftData` to locate the data chunk.
2. Read the chunk size as specified by the `size` field.
3. Validate the integrity using the CRC32C checksums.

---

## **Reading the `.dat` File**

1. **Parse the Header:**
   - Read the `DatHeader` to extract metadata and locate the MFT.

2. **Access the MFT:**
   - Seek to the `mft_offset`.
   - Parse the `MftHeader` and subsequent `MftData` entries.

3. **Locate and Read Data Chunks:**
   - Use the `offset` and `size` fields in `MftData` to locate the data chunks.
   - Validate data integrity using CRC32C checksums.

4. **Map Files to IDs:**
   - Use `MftIndexData` for file-to-ID mappings.

---

## **Checksum Calculation**
- CRC32C is used for verifying data integrity. Libraries or algorithms supporting CRC32C should be used to calculate and verify these checksums.

---

## **Usage Notes**
- The format is optimized for space and performance.
- Understanding the compression and checksum mechanisms is critical for extracting and interpreting the data correctly.
- Unknown fields may require further reverse engineering for complete understanding.

---

This documentation provides a foundational understanding of the `.dat` file format used in Guild Wars 2. For practical implementation, consider using binary file readers and checksum libraries compatible with the described structures.