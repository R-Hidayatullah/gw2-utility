#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <string>
#include <algorithm>
#include <cstdlib>

enum class MemberType
{
    Primitive,
    Custom,
    ArrayPrimitive,
    ArrayCustom,
    Unknown
};

struct StructMember
{
    std::string name;
    std::string type;
    MemberType memberTypeFirst;
    MemberType memberTypeSecond;
    std::string arraySize; // Default size for non-array members
};

std::string convertToRawStringLiteral(const std::string &normalString)
{
    // Try to find if there are any parentheses in the string
    size_t pos = normalString.find(')');
    std::string delimiter = "delimiter"; // Default delimiter

    if (pos != std::string::npos)
    {
        // If there are parentheses, use a custom delimiter
        delimiter = "customDelimiter"; // Choose a suitable delimiter name
    }

    return "R\"" + delimiter + "(" + normalString + ")" + delimiter + "\"";
}

std::string preprocessInput(const std::string &input)
{
    std::string processed;
    bool insideStruct = false;

    for (size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];

        if (c == '{')
        {
            insideStruct = true;
        }
        else if (c == '}')
        {
            insideStruct = false;
        }

        if (insideStruct && (c == '\n' || c == '\r'))
        {
            processed += ' '; // Replace newline with space
        }
        else
        {
            processed += c;
        }
    }

    return processed;
}

std::pair<std::string, std::vector<StructMember>> parseStruct(const std::string &structDef)
{
    std::regex structNameRegex(R"(\bstruct\s+(\w+)\s*\{)");
    std::smatch match;

    // Extract struct name
    std::string structName;
    if (std::regex_search(structDef, match, structNameRegex))
    {
        structName = match[1].str();
    }

    // Regular expression to match members, including array brackets
    std::regex memberRegex(R"(\s*([\w::<>]+)\s+([\w\d_]+)\s*(\[[^\]]*\])?\s*;)");
    std::vector<StructMember> members;
    std::string::const_iterator searchStart(structDef.cbegin());

    // Extract members of the struct
    while (std::regex_search(searchStart, structDef.cend(), match, memberRegex))
    {
        StructMember memberData;
        memberData.type = match[1].str();
        memberData.name = match[2].str();
        memberData.name.erase(std::remove(memberData.name.begin(), memberData.name.end(), '.'), memberData.name.end());

        if (memberData.type == "byte" ||
            memberData.type == "word" ||
            memberData.type == "dword" ||
            memberData.type == "qword" ||
            memberData.type == "int8" ||
            memberData.type == "int16" ||
            memberData.type == "int32" ||
            memberData.type == "int64" ||
            memberData.type == "uint8" ||
            memberData.type == "uint16" ||
            memberData.type == "uint32" ||
            memberData.type == "uint64" ||
            memberData.type == "char8" ||
            memberData.type == "char16" ||
            memberData.type == "char32" ||
            memberData.type == "ushort" ||
            memberData.type == "uint" ||
            memberData.type == "ulong" ||
            memberData.type == "float32" ||
            memberData.type == "float64" ||
            memberData.type == "float" ||
            memberData.type == "double")
        {
            memberData.memberTypeFirst = MemberType::Primitive;
            memberData.memberTypeSecond = MemberType::Unknown;
        }
        else if (memberData.type == "float2" ||
                 memberData.type == "float3" ||
                 memberData.type == "float4" ||
                 memberData.type == "word3" ||
                 memberData.type == "dword2" ||
                 memberData.type == "dword4" ||
                 memberData.type == "byte3" ||
                 memberData.type == "byte4" ||
                 memberData.type == "byte16")
        {
            memberData.memberTypeFirst = MemberType::Custom;
            memberData.memberTypeSecond = MemberType::Unknown;
        }
        else if (memberData.type.find("helpers::") != std::string::npos)
        {
            memberData.memberTypeFirst = MemberType::ArrayCustom;
            memberData.memberTypeSecond = MemberType::Unknown;
        }
        else
        {
            memberData.memberTypeFirst = MemberType::Custom;
            memberData.memberTypeSecond = MemberType::Unknown;
        }

        if (match[3].matched)
        {
            memberData.arraySize = match[3].str().substr(1, match[3].str().size() - 2);
            memberData.memberTypeSecond = MemberType::ArrayPrimitive;
        }

        members.push_back(memberData);
        searchStart = match.suffix().first;
    }

    return {structName, members};
}
std::string generateConstructor(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "::" << structName << "()\n";
    bool firstMember = true;

    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i].memberTypeFirst == MemberType::Primitive && members[i].memberTypeSecond == MemberType::Unknown)
        {
            if (firstMember)
            {
                oss << "    : ";
            }
            if (!firstMember)
            {
                oss << ", ";
            }
            oss << members[i].name << "(0)";
            firstMember = false;
        }
    }
    oss << " {}\n";

    return oss.str();
}

std::string generateCopyConstructor(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "::" << structName << "(const " << structName << "& p_other)\n";
    oss << "    : ";

    bool firstMember = true;
    for (const auto &member : members)
    {

        if ((member.memberTypeFirst != MemberType::ArrayCustom && member.memberTypeSecond == MemberType::Unknown) || (member.memberTypeFirst == MemberType::Unknown && member.memberTypeSecond != MemberType::ArrayPrimitive))
        {
            if (!firstMember)
            {
                oss << ", ";
            }
            oss << member.name << "(p_other." << member.name << ")";
            firstMember = false;
        }
    }
    oss << " {";
    for (const auto &member : members)
    {

        if ((member.memberTypeFirst == MemberType::ArrayCustom && member.memberTypeSecond != MemberType::Unknown) || (member.memberTypeFirst != MemberType::Unknown && member.memberTypeSecond == MemberType::ArrayPrimitive))
        {
            oss << "\nstd::copy(p_other." << member.name << ", p_other." << member.name << " + " << member.arraySize << ", " << member.name << ");";
        }
    }
    oss << "\n}\n";

    return oss.str();
}
std::string generateAssignmentOperator(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "& " << structDefinition << "::" << structName << "::operator=(const " << structName << "& p_other) {\n";
    for (const auto &member : members)
    {
        if (member.memberTypeFirst != MemberType::ArrayCustom && member.memberTypeSecond == MemberType::ArrayPrimitive)
        {
            oss << "    std::copy(p_other." << member.name << ", p_other." << member.name << " + " << member.arraySize << ", " << member.name << ");\n";
        }
        else
        {
            oss << "    " << member.name << " = p_other." << member.name << ";\n";
        }
    }
    oss << "    return *this;\n";
    oss << "}\n";

    return oss.str();
}
std::string generateAssignFunction(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << "const byte* " << structDefinition << "::" << structName << "::assign(const byte* p_data, size_t p_size) {\n";

    for (const auto &member : members)
    {

        if ((member.memberTypeFirst == MemberType::Primitive && member.memberTypeSecond == MemberType::ArrayPrimitive) || (member.memberTypeFirst == MemberType::Custom && member.memberTypeSecond == MemberType::ArrayPrimitive))
        {
            oss << "    std::copy(p_data, p_data + " << member.arraySize << ", " << member.name << ");\n";
        }
        else
        {
            oss << "    p_data = helpers::read(p_data, p_size, " << member.name << ");\n";
        }
    }

    oss << "    return p_data;\n";
    oss << "}\n";

    return oss.str();
}
std::string generateCustomConstructor(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "::" << structName << "(const byte* p_data, size_t p_size, const byte** po_pointer)\n";
    oss << " {\n";
    oss << "    auto pointer = assign(p_data, p_size);\n";
    oss << "    if (po_pointer)\n";
    oss << "    {\n";
    oss << "        *po_pointer = pointer;\n";
    oss << "    }\n";
    oss << "}\n";

    return oss.str();
}

// Function to generate OOP code for a specific struct version
std::string generateOOPCodeForStruct(const std::string &structDefinition, const std::string &structDef)
{
    // Parse the struct definition to extract name and members
    auto [structName, members] = parseStruct(structDef);

    // Generate the OOP code
    std::ostringstream oss;

    // Constructor
    oss << generateConstructor(structDefinition, structName, members);
    // Custom Constructor
    oss << generateCustomConstructor(structDefinition, structName, members);
    // Copy Constructor
    oss << generateCopyConstructor(structDefinition, structName, members);
    // Assignment Operator
    oss << generateAssignmentOperator(structDefinition, structName, members);
    // Assign function
    oss << generateAssignFunction(structDefinition, structName, members);

    return oss.str();
}

std::string processAllStructs(const std::string &input)
{
    // Preprocess input to handle multi-line definitions
    std::string processedInput = preprocessInput(input);

    // Regex to match each version block
    std::regex versionBlockRegex(R"(/[*]+\s*Version:\s*(\d+)\s*,.*?\*/\s*template\s*<>\s*struct\s*(Gw2Struct\w+<\d+>)\s*\{)");
    // /[*]+\s*Version:\s*(\d+)\s*,.*?\*/\s*template\s*<>\s*struct\s*(Gw2Struct\w+<\d+>)\s*\{([^*]*};)
    std::smatch match;

    std::string::const_iterator searchStart(processedInput.cbegin());
    std::string output;

    int counted = 0;

    while (std::regex_search(searchStart, processedInput.cend(), match, versionBlockRegex))
    {
        // Extract version and struct definition
        std::string version = match[1].str();
        std::string structDefinition = match[2].str();

        std::cout << "Processing version " << version << " for struct " << structDefinition << "\n";

        // Find the full block for the struct
        size_t startPos = match.suffix().first - processedInput.begin();
        size_t endPos = processedInput.find("typedef", startPos);
        if (endPos == std::string::npos)
        {
            throw std::runtime_error("Unmatched struct block!");
        }

        // Extract the full struct content
        std::string structBlock = processedInput.substr(startPos, endPos - startPos + 2);

        // Regex to match nested structs
        std::regex structDefRegex(R"(\bstruct\s+(\w+)\s*\{([^}]*)\})");
        std::smatch structMatch;
        std::string::const_iterator structSearchStart(structBlock.cbegin());
        while (std::regex_search(structSearchStart, structBlock.cend(), structMatch, structDefRegex))
        {
            std::string structName = structMatch[1].str();
            std::string structBody = structMatch[2].str();

            // std::cout << "Found struct: " << structName << "\n";

            // Generate OOP code for this struct
            output += generateOOPCodeForStruct(structDefinition, "struct " + structName + " {" + structBody + "};");

            structSearchStart = structMatch.suffix().first;
            counted++;
        }
        std::cout << "\nHave been found : " << counted << " structs\n\n";
        searchStart = processedInput.begin() + endPos + 2; // Move past the current block
    }

    return output;
}

// Helper function to create directories
void createDirectories(const std::string &path)
{
#ifdef _WIN32
    std::string command = "mkdir \"" + path + "\" > nul 2>&1";
#else
    std::string command = "mkdir -p \"" + path + "\"";
#endif
    system(command.c_str());
}

// Helper function to convert a string to lowercase
std::string toLowercase(const std::string &str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

void processFile(const std::string &inputFile, const std::string &outputDir)
{
    std::ifstream input(inputFile);
    if (!input.is_open())
    {
        std::cerr << "Error opening file: " << inputFile << "\n";
        return;
    }

    std::string line;
    std::string previousLine; // Variable to hold the previous line
    std::string currentChunk;
    int currentVersion = -1;
    std::ostringstream contentBuffer;
    std::regex chunkRegex(R"(Chunk:\s+(\w+))");
    std::regex versionRegex(R"(\/\* Version:\s+(\d+),)");

    while (std::getline(input, line))
    {
        std::smatch match;

        // If we find a chunk, save the previous line (if any) and the current line
        if (std::regex_search(line, match, chunkRegex))
        {
            // If there is content for the previous chunk, save it
            if (!currentChunk.empty())
            {
                std::string bufferContent = contentBuffer.str();

                // Check if the last line is the special comment and remove it
                if (bufferContent.find("/* ===============================================") != std::string::npos)
                {
                    size_t pos = bufferContent.rfind("\n/* ===============================================");
                    if (pos != std::string::npos)
                    {
                        bufferContent = bufferContent.substr(0, pos); // Remove the comment line
                    }
                }

                std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".h";
                createDirectories(outputDir + "/" + currentChunk);
                std::ofstream outFile(fileName);
                if (!outFile.is_open())
                {
                    std::cerr << "Error creating file: " << fileName << "\n";
                    continue;
                }

                // Add include guard
                std::string guardName = currentChunk;
                std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
                std::replace(guardName.begin(), guardName.end(), '.', '_');

                outFile << "#ifndef " << guardName << "_H\n";
                outFile << "#define " << guardName << "_H\n\n";
                outFile << "#include \"gw2formats/gw2_type.h\"\n\n";
                outFile << bufferContent; // Write the cleaned buffer content
                outFile << "\n#endif // " << guardName << "_H\n";

                outFile.close();
            }

            // Start new chunk with the previous line
            currentChunk = match[1];
            currentVersion = -1;   // Reset version for new chunk
            contentBuffer.str(""); // Clear the buffer
            contentBuffer.clear();

            if (!previousLine.empty())
            {
                contentBuffer << previousLine << "\n"; // Add the previous line before the chunk
            }
        }

        // Detect Version
        if (std::regex_search(line, match, versionRegex))
        {
            currentVersion = std::stoi(match[1]); // Update the version
        }

        // Append current line to content buffer
        contentBuffer << line << "\n";

        // Save the current line as the previous line for the next iteration
        previousLine = line;
    }

    // Write the last chunk
    if (!currentChunk.empty())
    {
        std::string bufferContent = contentBuffer.str();

        // Check if the last line is the special comment and remove it
        if (bufferContent.find("/* ===============================================") != std::string::npos)
        {
            size_t pos = bufferContent.rfind("\n/* ===============================================");
            if (pos != std::string::npos)
            {
                bufferContent = bufferContent.substr(0, pos); // Remove the comment line
            }
        }

        std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".h";
        createDirectories(outputDir + "/" + currentChunk);
        std::ofstream outFile(fileName);
        if (!outFile.is_open())
        {
            std::cerr << "Error creating file: " << fileName << "\n";
        }
        else
        {
            // Add include guard
            std::string guardName = currentChunk;
            std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
            std::replace(guardName.begin(), guardName.end(), '.', '_');

            outFile << "#ifndef " << guardName << "_H\n";
            outFile << "#define " << guardName << "_H\n\n";
            outFile << "#include \"gw2formats/gw2_type.h\"\n\n";
            outFile << bufferContent; // Write the cleaned buffer content
            outFile << "\n#endif // " << guardName << "_H\n";

            outFile.close();
        }
    }

    input.close();
}

void processFileSource(const std::string &inputFile, const std::string &outputDir)
{
    std::ifstream input(inputFile);
    if (!input.is_open())
    {
        std::cerr << "Error opening file: " << inputFile << "\n";
        return;
    }

    std::string line;
    std::string previousLine; // Variable to hold the previous line
    std::string currentChunk;
    int currentVersion = -1;
    std::ostringstream contentBuffer;
    std::regex chunkRegex(R"(Chunk:\s+(\w+))");
    std::regex versionRegex(R"(\/\* Version:\s+(\d+),)");

    while (std::getline(input, line))
    {
        std::smatch match;

        // If we find a chunk, save the previous line (if any) and the current line
        if (std::regex_search(line, match, chunkRegex))
        {
            // If there is content for the previous chunk, save it
            if (!currentChunk.empty())
            {
                std::string bufferContent = contentBuffer.str();

                // Check if the last line is the special comment and remove it
                if (bufferContent.find("/* ===============================================") != std::string::npos)
                {
                    size_t pos = bufferContent.rfind("\n/* ===============================================");
                    if (pos != std::string::npos)
                    {
                        bufferContent = bufferContent.substr(0, pos); // Remove the comment line
                    }
                }

                std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".cpp";
                createDirectories(outputDir + "/" + currentChunk);
                std::ofstream outFile(fileName);
                if (!outFile.is_open())
                {
                    std::cerr << "Error creating file: " << fileName << "\n";
                    continue;
                }

                // Add include guard
                std::string guardName = currentChunk;
                std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
                std::replace(guardName.begin(), guardName.end(), '.', '_');

                outFile << "#include \"gw2formats/fourcc/" << currentChunk << "/" << toLowercase(currentChunk) << ".h\"\n\n";
                outFile << "#include \"gw2formats/gw2_type.h\"\n\n";
                std::cout << "Parse content!\n";
                bufferContent = convertToRawStringLiteral(bufferContent);
                bufferContent = processAllStructs(bufferContent);
                outFile << bufferContent; // Write the cleaned buffer content

                outFile.close();
            }

            // Start new chunk with the previous line
            currentChunk = match[1];
            currentVersion = -1;   // Reset version for new chunk
            contentBuffer.str(""); // Clear the buffer
            contentBuffer.clear();

            if (!previousLine.empty())
            {
                contentBuffer << previousLine << "\n"; // Add the previous line before the chunk
            }
        }

        // Detect Version
        if (std::regex_search(line, match, versionRegex))
        {
            currentVersion = std::stoi(match[1]); // Update the version
        }

        // Append current line to content buffer
        contentBuffer << line << "\n";

        // Save the current line as the previous line for the next iteration
        previousLine = line;
    }

    // Write the last chunk
    if (!currentChunk.empty())
    {
        std::string bufferContent = contentBuffer.str();

        // Check if the last line is the special comment and remove it
        if (bufferContent.find("/* ===============================================") != std::string::npos)
        {
            size_t pos = bufferContent.rfind("\n/* ===============================================");
            if (pos != std::string::npos)
            {
                bufferContent = bufferContent.substr(0, pos); // Remove the comment line
            }
        }

        std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".cpp";
        createDirectories(outputDir + "/" + currentChunk);
        std::ofstream outFile(fileName);
        if (!outFile.is_open())
        {
            std::cerr << "Error creating file: " << fileName << "\n";
        }
        else
        {
            // Add include guard
            std::string guardName = currentChunk;
            std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
            std::replace(guardName.begin(), guardName.end(), '.', '_');

            outFile << "#include \"gw2formats/fourcc/" << currentChunk << "/" << toLowercase(currentChunk) << ".h\"\n\n";
            outFile << "#include \"gw2formats/gw2_type.h\"\n\n";
            std::cout << "Parse content!\n";

            bufferContent = convertToRawStringLiteral(bufferContent);
            bufferContent = processAllStructs(bufferContent);
            outFile << bufferContent; // Write the cleaned buffer content

            outFile.close();
        }
    }

    input.close();
}

int main()
{
    std::string inputFileHeader = "input_header.txt"; // Path to your input file
    std::string inputFileSource = "input_source.txt"; // Path to your input file
    std::string outputDirHeader = "include/fourcc";   // Directory to save generated files
    std::string outputDirSource = "source/fourcc";    // Directory to save generated files

    std::cout << "Started Parsing....\n";
    processFile(inputFileHeader, outputDirHeader);
    std::cout << "Finished\n";
    processFileSource(inputFileSource, outputDirSource);
    std::cout << "Finished\n";

    return 0;
}
