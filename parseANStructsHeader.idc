#include <idc.idc>

#define IS_ASCII(a) (((Byte(a) >= 48) && (Byte(a) <= 57)) || ((Byte(a) >= 65) && (Byte(a) <= 90)) || ((Byte(a) >= 97) && (Byte(a) <= 122)))

static getAsciiName(iAddress)
{
    auto aResult, aByte;
    aResult = "";
    while (aByte = Byte(iAddress))
    {
        aResult = form("%s%c", aResult, aByte);
        iAddress = iAddress + 1;
    }
    return aResult;
}

static add(iArrayId, iElement)
{
    auto aArrayId, aNextIndex;

    aNextIndex = GetLastIndex(AR_LONG, iArrayId) + 1;
    SetArrayLong(iArrayId, aNextIndex, iElement);
}

static isIn(iArrayId, iElement)
{
    auto aCurrentIndex;
    aCurrentIndex = GetFirstIndex(AR_LONG, iArrayId);

    while (aCurrentIndex != -1)
    {
        if (GetArrayElement(AR_LONG, iArrayId, aCurrentIndex) == iElement)
        {
            return 1;
        }

        aCurrentIndex = GetNextIndex(AR_LONG, iArrayId, aCurrentIndex);
    }
    return 0;
}

static isANStruct(iAddress)
{
    auto aCurrentAddress, aLoopGuard;

    aLoopGuard = 50;
    aCurrentAddress = iAddress;

    while (Word(aCurrentAddress) != 0 && aLoopGuard > 0)
    {
        if (Word(aCurrentAddress) > 0x1D)
        {
            return 0;
        }

        //Message("isANStruct: aCurrentAddress = 0x%X\n", aCurrentAddress);
        //aCurrentAddress = aCurrentAddress + 16;
        aCurrentAddress = aCurrentAddress + 32;
        aLoopGuard = aLoopGuard - 1;
        //Message("isANStruct: aCurrentAddress + 32 = 0x%X\n", aCurrentAddress);
    }

    //return (aLoopGuard != 0 && Dword(aCurrentAddress + 4) != BADADDR && IS_ASCII(Dword(aCurrentAddress + 4)));
    return (aLoopGuard != 0 && Qword(aCurrentAddress + 8) != BADADDR && IS_ASCII(Qword(aCurrentAddress + 8)));
}

static isANStructTab(iAddress, iNumber)
{
    auto aCurrentAddress, aLoopIndex;

    aLoopIndex = 0;
    aCurrentAddress = iAddress;

    while (aLoopIndex < iNumber)
    {
        //if (Dword(aCurrentAddress) != 0)
        if (Qword(aCurrentAddress) != 0)
        {
            //if (!isANStruct(Dword(aCurrentAddress)))
            if (!isANStruct(Qword(aCurrentAddress)))
            {
                break;
            }
        }
        //aCurrentAddress = aCurrentAddress + 12;
        aCurrentAddress = aCurrentAddress + 24;
        aLoopIndex = aLoopIndex + 1;
    }

    //Message("isANStructTab: iNumber = %d, aLoopIndex = %d\n", iNumber, aLoopIndex);
    return (aLoopIndex == iNumber);
}

static getSimpleTypeName(iAddress)
{
    auto aTypeId;
    aTypeId = Word(iAddress);
    if (aTypeId == 0x05)
        return "uint8_t";
    else if (aTypeId == 0x06)
        return "std::array<uint8_t, 4>";
    else if (aTypeId == 0x07)
        return "double";
    else if (aTypeId == 0x0A || aTypeId == 0x24 || aTypeId == 0x1C)
        return "uint32_t";
    else if (aTypeId == 0x0B)
        return "FilenameData";
    else if (aTypeId == 0x0C)
        return "float";
    else if (aTypeId == 0x0D)
        return "std::array<float, 2>";
    else if (aTypeId == 0x0E)
        return  "std::array<float, 3>";
    else if (aTypeId == 0x0F)
        return "std::array<float, 4>";
    else if (aTypeId == 0x11 || aTypeId == 0x25)
        return "uint64_t";
    else if (aTypeId == 0x12)
        return "WCharPtrData";
    else if (aTypeId == 0x13)
        return "CharPtrData";
    else if (aTypeId == 0x15)
        return "uint16_t";
    else if (aTypeId == 0x16)
        return "std::array<uint8_t, 16>";
    else if (aTypeId == 0x17)
        return "std::array<uint8_t, 3>";
    else if (aTypeId == 0x18)
        return "std::array<uint32_t, 2>";
    else if (aTypeId == 0x19)
        return "std::array<uint32_t, 4>";
    else if (aTypeId == 0x1A)
        return "std::array<uint16_t, 3>";
    else if (aTypeId == 0x1B)
        return "FileRefData";
    else
        return form("ERROR %d", aTypeId);
}

static parseMember(iAddress, iParsedStructsId, iOutputFile)
{
    auto aTypeId, aMemberName, aOptimized, aTempOutput;
    //aMemberName = getAsciiName(Dword(iAddress + 4));
    aMemberName = getAsciiName(Qword(iAddress + 8));
    aTypeId = Word(iAddress);

    if (aTypeId == 0x00)
    {
        aTempOutput = form("ERROR %s", aMemberName);
        Message("ERROR: Encountered 0x00 as a member typeId.");
        aOptimized = 1;
    }
    else if (aTypeId == 0x01)
    {
        //aTempOutput = form("std::array<%s, %d> %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile),  Dword(iAddress + 12), aMemberName);
        aTempOutput = form("std::array<%s, %d> %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile),  Qword(iAddress + 24), aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x02)
    {
        //aTempOutput = form("ArrayPtrData<%s> %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile), aMemberName);
        aTempOutput = form("ArrayPtrData<%s> %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile), aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x03)
    {
        //aTempOutput = form("PtrArrayPtrData<%s> %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile), aMemberName);
        aTempOutput = form("PtrArrayPtrData<%s> %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile), aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x04)
    {
        aTempOutput = form("%s %s", "Unknown0x04", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x05)
    {
        aTempOutput = form("%s %s", "uint8_t", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x06)
    {
        aTempOutput = form("%s %s", "std::array<uint8_t, 4>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x07)
    {
        aTempOutput = form("%s %s", "double", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x08)
    {
        aTempOutput = form("%s %s", "Unknown0x08", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x09)
    {
        aTempOutput = form("%s %s", "Unknown0x09", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0A || aTypeId == 0x24 || aTypeId == 0x1C)
    {
        aTempOutput = form("%s %s", "uint32_t", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0B)
    {
        aTempOutput = form("%s %s", "FilenameData", aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x0C)
    {
        aTempOutput = form("%s %s", "float", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0D)
    {
        aTempOutput = form("%s %s", "std::array<float, 2>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0E)
    {
        aTempOutput = form("%s %s", "std::array<float, 3>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0F)
    {
        aTempOutput = form("%s %s", "std::array<float, 4>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x10)
    {
        //aTempOutput = form("PtrData<%s> %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile), aMemberName);
        aTempOutput = form("PtrData<%s> %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile), aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x11 || aTypeId == 0x25)
    {
        aTempOutput = form("%s %s", "uint64_t", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x12)
    {
        aTempOutput = form("%s %s", "WCharPtrData", aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x13)
    {
        aTempOutput = form("%s %s", "CharPtrData", aMemberName);
        aOptimized = 0;
    }
    else if (aTypeId == 0x14)
    {
        //aTempOutput = form("%s %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile), aMemberName);
        aTempOutput = form("%s %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile), aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x15)
    {
        aTempOutput = form("%s %s", "uint16_t", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x16)
    {
        aTempOutput = form("%s %s", "std::array<uint8_t, 16>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x17)
    {
        aTempOutput = form("%s %s", "std::array<uint8_t, 3>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x18)
    {
        aTempOutput = form("%s %s", "std::array<uint32_t, 2>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x19)
    {
        aTempOutput = form("%s %s", "std::array<uint32_t, 4>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x1A)
    {
        aTempOutput = form("%s %s", "std::array<uint16_t, 3>", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x1B)
    {
        aTempOutput = form("%s %s", "FileRefData", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x1D)
    {
        //aTempOutput = form("%s %s", parseStruct(Dword(iAddress + 8), iParsedStructsId, iOutputFile), aMemberName);
        aTempOutput = form("%s %s", parseStruct(Qword(iAddress + 16), iParsedStructsId, iOutputFile), aMemberName);
        aOptimized = 1;
    }
    else
    {
        aTempOutput = form("ERROR %d %s",aTypeId, aMemberName);
        Message("ERROR: Encountered > 0x1D as a member typeId = %d\n",aTypeId);
        aOptimized = 1;
    }

    aTempOutput = form("    %s;\n", aTempOutput);

    return aTempOutput;
}


static getSimpleTypeNameLoad(iAddress,aMemberName)
{
    auto aTypeId;
    aTypeId = Word(iAddress);
    if (aTypeId == 0x02)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x03)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x05)
        return aMemberName+"[i] = r.read<uint8_t>()";
    else if (aTypeId == 0x06)
        return "for(int i = 0; i < 4; ++i){\n"+aMemberName+"[i]= r.read<uint8_t>();\n}";
    else if (aTypeId == 0x07)
        return aMemberName+"[i] = r.read<double>()";
    else if (aTypeId == 0x0A || aTypeId == 0x24 || aTypeId == 0x1C)
        return aMemberName+"[i] = r.read<uint32_t>()";
    else if (aTypeId == 0x0B)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x0C)
        return aMemberName+"[i] = r.read<float>()";
    else if (aTypeId == 0x0D)
        return "for(int i = 0; i < 2; ++i){\n"+aMemberName+"[i]= r.read<float>();\n}";
    else if (aTypeId == 0x0E)
        return "for(int i = 0; i < 3; ++i){\n"+aMemberName+"[i]= r.read<float>();\n}";
    else if (aTypeId == 0x0F)
        return "for(int i = 0; i < 4; ++i){\n"+aMemberName+"[i]= r.read<float>();\n}";
    else if (aTypeId == 0x10)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x11 || aTypeId == 0x25)
        return aMemberName+"[i] = r.read<uint64_t>()";
    else if (aTypeId == 0x12)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x13)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x14)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x15)
        return aMemberName+"[i] = r.read<uint16_t>()";
    else if (aTypeId == 0x16)
        return "for(int i = 0; i < 16; ++i){\n"+aMemberName+"[i]= r.read<uint8_t>();\n}";
    else if (aTypeId == 0x17)
        return "for(int i = 0; i < 3; ++i){\n"+aMemberName+"[i]= r.read<uint8_t>();\n}";
    else if (aTypeId == 0x18)
        return "for(int i = 0; i < 2; ++i){\n"+aMemberName+"[i]= r.read<uint32_t>();\n}";
    else if (aTypeId == 0x19)
        return "for(int i = 0; i < 4; ++i){\n"+aMemberName+"[i]= r.read<uint32_t>();\n}";
    else if (aTypeId == 0x1A)
        return "for(int i = 0; i < 3; ++i){\n"+aMemberName+"[i]= r.read<uint16_t>();\n}";
    else if (aTypeId == 0x1B)
        return aMemberName+"[i].load(r)";
    else if (aTypeId == 0x1D)
        return aMemberName+"[i].load(r)";
    else
        return form("ERROR %d", aTypeId);
}

static parseMemberLoad(iAddress, iParsedStructsId, iOutputFile)
{
    auto aTypeId, aMemberName, aOptimized, aTempOutput;
    //aMemberName = getAsciiName(Dword(iAddress + 4));
    aMemberName = getAsciiName(Qword(iAddress + 8));
    aTypeId = Word(iAddress);

    if (aTypeId == 0x00)
    {
        aTempOutput = form("ERROR %s", aMemberName);
        Message("ERROR: Encountered 0x00 as a member typeId.");
        aOptimized = 1;
    }
    else if (aTypeId == 0x01)
    {
        aTempOutput = form("for(int i = 0; i < %d; ++i){\n%s\n}", Qword(iAddress + 24), getSimpleTypeNameLoad(Qword(iAddress + 16), aMemberName) );
        aOptimized = 1;
    }
    else if (aTypeId == 0x02)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 0;
    }
    else if (aTypeId == 0x03)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 0;
    }
    else if (aTypeId == 0x04)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "Unknown0x04" );
        aOptimized = 1;
    }
    else if (aTypeId == 0x05)
    {
        aTempOutput = form("%s = r.read<%s>()",aMemberName , "uint8_t");
        aOptimized = 1;
    }
    else if (aTypeId == 0x06)
    {
        aTempOutput = form("for(int i = 0; i < 4; ++i){\n%s[i]= r.read<uint8_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x07)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "double");
        aOptimized = 1;
    }
    else if (aTypeId == 0x08)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "Unknown0x08");
        aOptimized = 1;
    }
    else if (aTypeId == 0x09)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "Unknown0x09");
        aOptimized = 1;
    }
    else if (aTypeId == 0x0A || aTypeId == 0x24 || aTypeId == 0x1C)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "uint32_t");
        aOptimized = 1;
    }
    else if (aTypeId == 0x0B)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "FilenameData");
        aOptimized = 0;
    }
    else if (aTypeId == 0x0C)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "float");
        aOptimized = 1;
    }
    else if (aTypeId == 0x0D)
    {
        aTempOutput = form("for(int i = 0; i < 2; ++i){\n%s[i]= r.read<float>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x0E)
    {
        aTempOutput = form("for(int i = 0; i < 3; ++i){\n%s[i]= r.read<float>();\n}", aMemberName);

        aOptimized = 1;
    }
    else if (aTypeId == 0x0F)
    {
        aTempOutput = form("for(int i = 0; i < 4; ++i){\n%s[i]= r.read<float>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x10)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 0;
    }
    else if (aTypeId == 0x11 || aTypeId == 0x25)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "uint64_t");
        aOptimized = 1;
    }
    else if (aTypeId == 0x12)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 0;
    }
    else if (aTypeId == 0x13)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 0;
    }
    else if (aTypeId == 0x14)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 1;
    }
    else if (aTypeId == 0x15)
    {
        aTempOutput = form("%s = r.read<%s>()", aMemberName, "uint16_t");
        aOptimized = 1;
    }
    else if (aTypeId == 0x16)
    {
        aTempOutput = form("for(int i = 0; i < 16; ++i){\n%s[i]= r.read<uint8_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x17)
    {
        aTempOutput = form("for(int i = 0; i < 3; ++i){\n%s[i]= r.read<uint8_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x18)
    {
        aTempOutput = form("for(int i = 0; i < 2; ++i){\n%s[i]= r.read<uint32_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x19)
    {
        aTempOutput = form("for(int i = 0; i < 4; ++i){\n%s[i]= r.read<uint32_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x1A)
    {
        aTempOutput = form("for(int i = 0; i < 3; ++i){\n%s[i]= r.read<uint16_t>();\n}", aMemberName);
        aOptimized = 1;
    }
    else if (aTypeId == 0x1B)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 1;
    }
    else if (aTypeId == 0x1D)
    {
        aTempOutput = form("%s%s", aMemberName, ".load(r)");
        aOptimized = 1;
    }
    else
    {
        aTempOutput = form("ERROR %d %s",aTypeId, aMemberName);
        Message("ERROR: Encountered > 0x1D as a member typeId = %d\n",aTypeId);
        aOptimized = 1;
    }

    aTempOutput = form("    %s;\n", aTempOutput);

    return aTempOutput;
}

static parseStruct(iAddress, iParsedStructsId, iOutputFile)
{
    auto aOutput, aOutputLoad, aStructName, aCurrentAddress, aAlreadyParsed, aMemberOutput, aMemberOutputLoad;

    aAlreadyParsed = isIn(iParsedStructsId, iAddress);
    add(iParsedStructsId, iAddress);

    aCurrentAddress = iAddress;

    // Special case for simple types
    //if (Byte(Dword(aCurrentAddress + 4)) == 0)
    if (Byte(Qword(aCurrentAddress + 8)) == 0)
    {
        return getSimpleTypeName(iAddress);
    }

    while (Word(aCurrentAddress) != 0)
    {
        if (!aAlreadyParsed)
        {
            aMemberOutput = parseMember(aCurrentAddress, iParsedStructsId, iOutputFile);
            aOutput = form("%s%s", aOutput, aMemberOutput);

            aMemberOutputLoad = parseMemberLoad(aCurrentAddress, iParsedStructsId, iOutputFile);
            aOutputLoad=form("%s%s", aOutputLoad, aMemberOutputLoad);
        }

        //aCurrentAddress = aCurrentAddress + 16;
        aCurrentAddress = aCurrentAddress + 32;
    }

    //aStructName = getAsciiName(Dword(aCurrentAddress + 4));
    aStructName = getAsciiName(Qword(aCurrentAddress + 8));

    if (!aAlreadyParsed)
    {
        aOutput = form("struct %s {\n%s\nvoid load(BufferReader &r){\n%s\n}\n} ;\n\n", aStructName, aOutput, aOutputLoad);
        fprintf(iOutputFile, "%s", aOutput);
    }

    return aStructName;
}

static parseStructTab(iANSTructTabOffset, iNbOfVersions, iOutputFile)
{
    auto aCurrentAddress, aLoopIndex, aParsedStructsId, aSubAddress;

    aLoopIndex = iNbOfVersions - 1;
    aCurrentAddress = iANSTructTabOffset;

    while (aLoopIndex >= 0)
    {
        DeleteArray(GetArrayId("PARSED_STRUCTS"));
        aParsedStructsId = CreateArray("PARSED_STRUCTS");

        //aCurrentAddress = Dword(iANSTructTabOffset + 12 * aLoopIndex);
        //aSubAddress = Dword(iANSTructTabOffset + 12 * aLoopIndex + 4);
        aCurrentAddress = Qword(iANSTructTabOffset + 24 * aLoopIndex);
        aSubAddress = Qword(iANSTructTabOffset + 24 * aLoopIndex + 4);
        if (aCurrentAddress !=0)
        {
            if (aSubAddress != 0)
            {
                fprintf(iOutputFile, "/* Version: %d, ReferencedFunction: 0x%X */\n", aLoopIndex, aSubAddress);
            }
            else
            {
                fprintf(iOutputFile, "/* Version: %d */\n", aLoopIndex);
            }
            fprintf(iOutputFile, "namespace version%d {\n\n", aLoopIndex);
            parseStruct(aCurrentAddress, aParsedStructsId, iOutputFile);
            fprintf(iOutputFile, "}\n", aLoopIndex);
        }
        aLoopIndex = aLoopIndex - 1;
    }
}

static main(void)
{
    auto aParsedTablesId;

    // First step detecting rdata and text segments
    auto aCurrentSeg, aCurrentAddress, aMiscAddress;
    auto aMinDataSeg, aMaxDataSeg;
    auto aMinRDataSeg, aMaxRDataSeg;
    auto aMinTextSeg, aMaxTextSeg;

    auto aChunkName, aNbOfVersions, aANSTructTabOffset;

    auto aOutputFile, aReportFile;
    aOutputFile = fopen("output.txt", "w");

    Message("ANet structs script started.\n");

    aMinDataSeg = 0;
    aMaxDataSeg = 0;
    aMinRDataSeg = 0;
    aMaxRDataSeg = 0;
    aMinTextSeg = 0;
    aMaxTextSeg = 0;

    DeleteArray(GetArrayId("PARSED_TABLES"));
    aParsedTablesId = CreateArray("PARSED_TABLES");

    aCurrentSeg = FirstSeg();

    while (aCurrentSeg != BADADDR)
    {
        if (SegName(aCurrentSeg)==".rdata")
        {
            aMinRDataSeg = aCurrentSeg;
            aMaxRDataSeg = NextSeg(aCurrentSeg);
        }
        else if (SegName(aCurrentSeg)==".text")
        {
            aMinTextSeg = aCurrentSeg;
            aMaxTextSeg = NextSeg(aCurrentSeg);
        }
        else if (SegName(aCurrentSeg)==".data")
        {
            aMinDataSeg = aCurrentSeg;
            aMaxDataSeg = NextSeg(aCurrentSeg);
        }
        aCurrentSeg = NextSeg(aCurrentSeg);
    }

    if (aMinRDataSeg == 0)
    {
        aMinRDataSeg=aMinTextSeg;
        aMaxRDataSeg=aMaxTextSeg;
    }

    Message(".data: %08.8Xh - %08.8Xh, .rdata: %08.8Xh - %08.8Xh, .text %08.8Xh - %08.8Xh\n", aMinDataSeg, aMaxDataSeg, aMinRDataSeg, aMaxRDataSeg, aMinTextSeg, aMaxTextSeg);

    Message("Parsing .rdata for chunk_infos.\n");

    aCurrentAddress = aMinRDataSeg;
    while (aCurrentAddress < aMaxRDataSeg)
    {
        if (IS_ASCII(aCurrentAddress) && IS_ASCII(aCurrentAddress + 1) && IS_ASCII(aCurrentAddress + 2) && (Byte(aCurrentAddress + 3) == 0 || IS_ASCII(aCurrentAddress + 3)))
        {
            aChunkName = form("%c%c%c", Byte(aCurrentAddress), Byte(aCurrentAddress + 1), Byte(aCurrentAddress + 2));
            if (Byte(aCurrentAddress + 3) != 0)
            {
                aChunkName = form("%s%c", aChunkName, Byte(aCurrentAddress + 3));
            }

            aNbOfVersions = Dword(aCurrentAddress + 4);
            if (aNbOfVersions > 0 && aNbOfVersions < 100)
            {
                //aANSTructTabOffset = Dword(aCurrentAddress + 8);
                aANSTructTabOffset = Qword(aCurrentAddress + 8);
                if ((aMinRDataSeg < aANSTructTabOffset) && (aMaxRDataSeg > aANSTructTabOffset))
                {
                    Message("Address: %08.8Xh, ChunkName:%s, versions: %d, strucTab: 0x%X\n", aCurrentAddress, aChunkName, aNbOfVersions, aANSTructTabOffset);
                    if (isANStructTab(aANSTructTabOffset, aNbOfVersions))
                    {
                        if (!isIn(aParsedTablesId, aANSTructTabOffset))
                        {
                            add(aParsedTablesId, aANSTructTabOffset);
                            fprintf(aOutputFile, "/* ===============================================\n");
                            fprintf(aOutputFile, " * Chunk: %s, versions: %d, strucTab: 0x%X\n", aChunkName, aNbOfVersions, aANSTructTabOffset);
                            fprintf(aOutputFile, " * ===============================================\n");
                            fprintf(aOutputFile, " */\n");
                            fprintf(aOutputFile, "namespace %s {\n\n",aChunkName);
                            parseStructTab(aANSTructTabOffset, aNbOfVersions, aOutputFile);
                            fprintf(aOutputFile, "}\n");
                            fprintf(aOutputFile, "\n");
                        }
                    }
                }
            }
        }

        aCurrentAddress = aCurrentAddress + 4;
    }

    DeleteArray(aParsedTablesId);

    Message("ANet structs script ended.\n");

    fclose(aOutputFile);

}