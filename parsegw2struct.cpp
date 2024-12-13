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
    Array,
    Helpers
};

struct StructMember
{
    std::string name;
    std::string type;
    MemberType memberType;
    int arraySize = 0; // Default size for non-array members
};

StructMember parseMember(const std::string &declaration)
{
    std::regex arrayRegex(R"((\w+)\s+(\w+)\[(\d+)\];)");
    std::regex PrimitiveRegex(R"((\w+)\s+(\w+);)");

    std::smatch match;
    StructMember member;

    if (std::regex_match(declaration, match, arrayRegex))
    {
        member.type = match[1].str();
        member.name = match[2].str();
        member.memberType = MemberType::Array;
        member.arraySize = std::stoi(match[3].str());
    }
    else if (std::regex_match(declaration, match, PrimitiveRegex))
    {
        member.type = match[1].str();
        member.name = match[2].str();
        member.memberType = MemberType::Primitive;
    }
    else
    {
        throw std::runtime_error("Unsupported member declaration: " + declaration);
    }

    return member;
}

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
        StructMember member;
        member.type = match[1].str();
        member.name = match[2].str();

        // Determine member type
        if (member.type.find("helpers::") != std::string::npos)
        {
            member.memberType = MemberType::Helpers;
        }
        else if (match[3].matched) // Array detection
        {
            member.name += match[3].str(); // Append array part to the name
            member.memberType = MemberType::Array;
        }
        else
        {
            member.memberType = MemberType::Primitive;
        }

        members.push_back(member);
        searchStart = match.suffix().first;
    }

    return {structName, members};
}
std::string generateConstructor(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "::" << structName << "()\n";
    oss << "    : ";

    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i].memberType != MemberType::Helpers)
        {
            oss << members[i].name << "(0)";
            if (i != members.size() - 1)
            {
                oss << ", ";
            }
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
        if (member.memberType == MemberType::Helpers)
        {
            continue;
        }

        if (!firstMember)
        {
            oss << ", ";
        }

        if (member.memberType == MemberType::Array)
        {
            oss << "std::copy(p_other." << member.name << ", p_other." << member.name << " + " << member.arraySize << ", " << member.name << ")";
        }
        else
        {
            oss << member.name << "(p_other." << member.name << ")";
        }

        firstMember = false;
    }
    oss << " {}\n";

    return oss.str();
}
std::string generateAssignmentOperator(const std::string &structDefinition, const std::string &structName, const std::vector<StructMember> &members)
{
    std::ostringstream oss;
    oss << structDefinition << "::" << structName << "& " << structName << "::operator=(const " << structName << "& p_other) {\n";
    for (const auto &member : members)
    {
        if (member.memberType == MemberType::Array)
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

        if (member.memberType == MemberType::Array)
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
    oss << "    : ";

    // Generate the member initializations
    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i].memberType != MemberType::Helpers)
        {
            oss << members[i].name << "(0)";
            if (i != members.size() - 1)
            {
                oss << ", ";
            }
        }
    }
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
    std::smatch match;

    std::string::const_iterator searchStart(processedInput.cbegin());
    std::string output;

    while (std::regex_search(searchStart, processedInput.cend(), match, versionBlockRegex))
    {
        // Extract version and struct definition
        std::string version = match[1].str();
        std::string structDefinition = match[2].str();

        std::cout << "Processing version " << version << " for struct " << structDefinition << "\n";

        // Find the full block for the struct
        size_t startPos = match.suffix().first - processedInput.begin();
        size_t endPos = processedInput.find("};", startPos);
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

            std::cout << "Found struct: " << structName << "\n";

            // Generate OOP code for this struct
            output += generateOOPCodeForStruct(structDefinition, "struct " + structName + " {" + structBody + "};");

            structSearchStart = structMatch.suffix().first;
        }

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
