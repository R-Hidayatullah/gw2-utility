from inc_noesis import *
import noesis
import rapi

def registerNoesisTypes():
    """Register the plugin with Noesis."""
    handle = noesis.register("Guild Wars 2 Model", ".pf")
    noesis.setHandlerTypeCheck(handle, gw2CheckType)
    noesis.setHandlerLoadModel(handle, gw2LoadModel)
    return 1

def gw2CheckType(data):
    """Check if the file is a valid GW2 model file."""
    if len(data) < 12:
        return 0
    
    # Check for PF header
    if data[:2] != b'PF':
        return 0
    
    return 1

class GW2Parser:
    def __init__(self, data):
        self.data = data
        self.bs = NoeBitStream(data)
        self.is64bit = False
        self.ptrSize = 4
        self.detectPointerSize()
    
    def detectPointerSize(self):
        """Detect if the file uses 32-bit or 64-bit pointers."""
        # Save current position
        pos = self.bs.tell()
        
        # Skip header to find first chunk
        self.bs.seek(0)
        magic = self.bs.readBytes(2)  # PF
        version = self.bs.readUShort()
        zero = self.bs.readUShort()
        headerSize = self.bs.readUShort()
        fileType = self.bs.readBytes(4)
        
        # Read first chunk header
        if self.bs.tell() < len(self.data) - 16:
            chunkMagic = self.bs.readBytes(4)
            chunkSize = self.bs.readUInt()
            chunkVersion = self.bs.readUShort()
            chunkHeaderSize = self.bs.readUShort()
            offsetToOffsetTable = self.bs.readUInt()
            
            # Heuristic: check if offset table position makes sense
            # For 64-bit files, pointers will be larger and offsets will be different
            if offsetToOffsetTable > 0 and offsetToOffsetTable < chunkSize:
                # Check pattern of data to determine pointer size
                dataStart = self.bs.tell()
                if dataStart + 12 < len(self.data):
                    # Read what would be a count + pointer combination
                    count = self.bs.readUInt()
                    ptr1 = self.bs.readUInt()
                    ptr2 = self.bs.readUInt()
                    
                    # In 64-bit files, the second uint32 is the high part of a 64-bit pointer
                    # If ptr2 is 0 and ptr1 is reasonable, likely 64-bit
                    # If ptr1 is very large or ptr2 is non-zero, likely 64-bit
                    if ptr2 == 0 and ptr1 > 0 and ptr1 < len(self.data):
                        # Could be 64-bit with high part zero
                        self.is64bit = True
                        self.ptrSize = 8
                    elif count > 0 and count < 100000 and ptr1 > 0 and ptr1 < len(self.data) and ptr2 == 0:
                        # Strong indication of 64-bit
                        self.is64bit = True
                        self.ptrSize = 8
        
        # Restore position
        self.bs.seek(pos)
        
        print(f"Detected pointer size: {'64-bit' if self.is64bit else '32-bit'}")
    
    def readPointer(self):
        """Read a pointer based on detected architecture."""
        if self.is64bit:
            return self.bs.readUInt64()
        else:
            return self.bs.readUInt()
    
    def readHeader(self):
        """Read the main file header."""
        magic = self.bs.readBytes(2)
        version = self.bs.readUShort()
        zero = self.bs.readUShort()
        headerSize = self.bs.readUShort()
        fileType = self.bs.readBytes(4)
        
        return {
            'magic': magic,
            'version': version,
            'zero': zero,
            'headerSize': headerSize,
            'fileType': fileType
        }
    
    def readChunkHeader(self):
        """Read a chunk header."""
        magic = self.bs.readBytes(4)
        chunkSize = self.bs.readUInt()
        version = self.bs.readUShort()
        headerSize = self.bs.readUShort()
        offsetToOffsetTable = self.bs.readUInt()
        
        return {
            'magic': magic,
            'chunkSize': chunkSize,
            'version': version,
            'headerSize': headerSize,
            'offsetToOffsetTable': offsetToOffsetTable
        }
    
    def readArrayPtr(self, elementReader, baseOffset):
        """Read an array pointer structure."""
        count = self.bs.readUInt()
        offset = self.readPointer()
        
        if offset == 0 or count == 0:
            return []
        
        # Save current position
        savedPos = self.bs.tell()
        
        # Seek to array data
        self.bs.seek(baseOffset + offset)
        
        # Read array elements
        elements = []
        for i in range(count):
            elements.append(elementReader())
        
        # Restore position
        self.bs.seek(savedPos)
        
        return elements
    
    def readPtrArrayPtr(self, elementReader, baseOffset):
        """Read a pointer array pointer structure."""
        count = self.bs.readUInt()
        offset = self.readPointer()
        
        if offset == 0 or count == 0:
            return []
        
        # Save current position
        savedPos = self.bs.tell()
        
        # Seek to pointer array
        self.bs.seek(baseOffset + offset)
        
        # Read array of pointers
        elements = []
        for i in range(count):
            ptrOffset = self.readPointer()
            if ptrOffset != 0:
                ptrSavedPos = self.bs.tell()
                self.bs.seek(baseOffset + ptrOffset)
                elements.append(elementReader(baseOffset))
                self.bs.seek(ptrSavedPos)
        
        # Restore position
        self.bs.seek(savedPos)
        
        return elements
    
    def readPtr(self, elementReader, baseOffset):
        """Read a pointer structure."""
        offset = self.readPointer()
        
        if offset == 0:
            return None
        
        # Save current position
        savedPos = self.bs.tell()
        
        # Seek to data
        self.bs.seek(baseOffset + offset)
        element = elementReader(baseOffset)
        
        # Restore position
        self.bs.seek(savedPos)
        
        return element
    
    def readFloat3(self):
        """Read a float3 vector."""
        return [self.bs.readFloat(), self.bs.readFloat(), self.bs.readFloat()]
    
    def readCharPtr(self, baseOffset):
        """Read a char pointer."""
        offset = self.readPointer()
        if offset == 0:
            return ""
        
        savedPos = self.bs.tell()
        self.bs.seek(baseOffset + offset)
        result = self.bs.readString()
        self.bs.seek(savedPos)
        return result
    
    def readModelMeshMorphVert(self, version):
        """Read ModelMeshMorphVert based on version."""
        index = self.bs.readUShort()
        vector = self.readFloat3()
        return {'index': index, 'vector': vector}
    
    def readModelMeshMorphTarget(self, version, baseOffset):
        """Read ModelMeshMorphTarget based on version."""
        positions = self.readArrayPtr(lambda: self.readModelMeshMorphVert(version), baseOffset)
        normals = self.readArrayPtr(lambda: self.readModelMeshMorphVert(version), baseOffset)
        mesh = self.bs.readUInt64()  # qword
        
        return {
            'positions': positions,
            'normals': normals,
            'mesh': mesh
        }
    
    def readPackVertexType(self, baseOffset):
        """Read PackVertexType structure."""
        fvf = self.bs.readUInt()
        vertices = self.readArrayPtr(lambda: self.bs.readUByte(), baseOffset)
        
        return {
            'fvf': fvf,
            'vertices': vertices
        }
    
    def readModelMeshVertexData(self, version, baseOffset):
        """Read ModelMeshVertexData based on version."""
        vertexCount = self.bs.readUInt()
        mesh = self.readPackVertexType(baseOffset)
        
        return {
            'vertexCount': vertexCount,
            'mesh': mesh
        }
    
    def readModelMeshIndexData(self, version, baseOffset):
        """Read ModelMeshIndexData based on version."""
        indices = self.readArrayPtr(lambda: self.bs.readUShort(), baseOffset)
        return {'indices': indices}
    
    def readModelMeshGeometry(self, version, baseOffset):
        """Read ModelMeshGeometry based on version."""
        verts = self.readModelMeshVertexData(version, baseOffset)
        indices = self.readModelMeshIndexData(version, baseOffset)
        lods = self.readArrayPtr(lambda: self.readModelMeshIndexData(version, baseOffset), baseOffset)
        
        result = {
            'verts': verts,
            'indices': indices,
            'lods': lods
        }
        
        if version >= 1:
            transforms = self.readArrayPtr(lambda: self.bs.readUInt(), baseOffset)
            result['transforms'] = transforms
        
        return result
    
    def readGrBoundData(self):
        """Read GrBoundData structure."""
        center = self.readFloat3()
        boxExtent = self.readFloat3()
        sphereRadius = self.bs.readFloat()
        
        return {
            'center': center,
            'boxExtent': boxExtent,
            'sphereRadius': sphereRadius
        }
    
    def readModelMeshData(self, version, baseOffset):
        """Read ModelMeshData based on version."""
        visBone = self.bs.readUInt64()  # qword
        
        # Read morph targets
        if version >= 66:
            morphTargets = self.readArrayPtr(lambda: self.readModelMeshMorphTarget(66, baseOffset), baseOffset)
        else:
            morphTargets = self.readArrayPtr(lambda: self.readModelMeshMorphTarget(65, baseOffset), baseOffset)
        
        flags = self.bs.readUInt()
        seamVertIndices = self.readArrayPtr(lambda: self.bs.readUInt(), baseOffset)
        meshName = self.bs.readUInt64()  # qword
        minBound = self.readFloat3()
        maxBound = self.readFloat3()
        bounds = self.readArrayPtr(lambda: self.readGrBoundData(), baseOffset)
        materialIndex = self.bs.readUInt()
        materialName = self.readCharPtr(baseOffset)
        
        if version >= 66:
            boneBindings = self.readArrayPtr(lambda: self.bs.readUInt64(), baseOffset)
        else:
            boneNames = self.readArrayPtr(lambda: self.readCharPtr(baseOffset), baseOffset)
        
        geometry = self.readPtr(lambda bo: self.readModelMeshGeometry(1 if version >= 66 else 0, bo), baseOffset)
        
        result = {
            'visBone': visBone,
            'morphTargets': morphTargets,
            'flags': flags,
            'seamVertIndices': seamVertIndices,
            'meshName': meshName,
            'minBound': minBound,
            'maxBound': maxBound,
            'bounds': bounds,
            'materialIndex': materialIndex,
            'materialName': materialName,
            'geometry': geometry
        }
        
        if version >= 66:
            result['boneBindings'] = boneBindings
        else:
            result['boneNames'] = boneNames
        
        return result
    
    def readModelFileGeometry(self, version, baseOffset):
        """Read ModelFileGeometry based on version."""
        if version >= 1:
            meshes = self.readPtrArrayPtr(lambda bo: self.readModelMeshData(66, bo), baseOffset)
        else:
            meshes = self.readPtrArrayPtr(lambda bo: self.readModelMeshData(65, bo), baseOffset)
        
        return {'meshes': meshes}
    
    def parseGEOMChunk(self, chunkHeader, baseOffset):
        """Parse a GEOM chunk based on version."""
        version = chunkHeader['version']
        print(f"Parsing GEOM chunk version {version}")
        
        geometry = self.readModelFileGeometry(version, baseOffset)
        
        return geometry

def gw2LoadModel(data, mdlList):
    """Main function to load GW2 model."""
    try:
        parser = GW2Parser(data)
        
        # Read main header
        header = parser.readHeader()
        print(f"File type: {header['fileType']}")
        
        # Skip to first chunk
        parser.bs.seek(12)  # Skip main header
        
        # Parse chunks
        while parser.bs.tell() < len(data) - 16:
            chunkStart = parser.bs.tell()
            chunkHeader = parser.readChunkHeader()
            
            print(f"Found chunk: {chunkHeader['magic']} (version {chunkHeader['version']})")
            
            if chunkHeader['magic'] == b'GEOM':
                # Parse geometry chunk
                geometry = parser.parseGEOMChunk(chunkHeader, chunkStart)
                
                # Convert to Noesis format
                meshes = convertToNoesisMeshes(geometry)
                for mesh in meshes:
                    mdlList.append(mesh)
            else:
                # Skip unknown chunks
                parser.bs.seek(chunkStart + chunkHeader['chunkSize'] + 8)
        
        return 1
        
    except Exception as e:
        print(f"Error parsing GW2 model: {e}")
        return 0

def convertToNoesisMeshes(geometry):
    """Convert parsed geometry to Noesis meshes."""
    meshes = []
    
    if not geometry or 'meshes' not in geometry:
        return meshes
    
    for i, meshData in enumerate(geometry['meshes']):
        if not meshData or not meshData.get('geometry'):
            continue
        
        geom = meshData['geometry']
        
        # Extract vertices
        if not geom.get('verts') or not geom['verts'].get('mesh'):
            continue
        
        vertexData = geom['verts']['mesh']['vertices']
        vertexCount = geom['verts']['vertexCount']
        
        if not vertexData or vertexCount == 0:
            continue
        
        # Extract indices
        indices = []
        if geom.get('indices') and geom['indices'].get('indices'):
            indices = geom['indices']['indices']
        
        # For now, create a simple mesh (vertex format parsing would need more work)
        # This is a basic implementation - full vertex format parsing would be more complex
        
        try:
            # Create basic vertex positions (this would need proper FVF parsing)
            positions = []
            for j in range(0, min(len(vertexData), vertexCount * 12), 12):  # Assuming 12 bytes per vertex (3 floats)
                if j + 11 < len(vertexData):
                    x = noesis.bytesToFloat(vertexData[j:j+4])
                    y = noesis.bytesToFloat(vertexData[j+4:j+8])
                    z = noesis.bytesToFloat(vertexData[j+8:j+12])
                    positions.extend([x, y, z])
            
            if len(positions) >= 9:  # At least one triangle
                # Create mesh
                mesh = NoeMesh(indices if indices else [], positions, f"mesh_{i}")
                
                # Set material name if available
                if meshData.get('materialName'):
                    mesh.setMaterial(meshData['materialName'])
                
                meshes.append(mesh)
        
        except Exception as e:
            print(f"Error converting mesh {i}: {e}")
            continue
    
    return meshes