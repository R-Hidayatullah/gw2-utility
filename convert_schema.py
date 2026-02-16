import re
import json

def parse_txt_to_json(txt_file):
    with open(txt_file, "r", encoding="utf-8") as f:
        lines = f.readlines()

    schema = {
        "schemaVersion": 1,
        "chunks": []
    }

    current_chunk = None
    current_version = None
    struct_stack = []

    # Regex patterns
    struct_start_re = re.compile(r"typedef struct \{")
    struct_end_re = re.compile(r"\} (\w+);")
    member_re = re.compile(
        r"(TSTRUCT_ARRAY_PTR_START|TSTRUCT_PTR_ARRAY_PTR_START|TPTR_START)?\s*(\w+)\s+(\w+)(?:\s*\[.*\])?\s*(TSTRUCT_ARRAY_PTR_END|TSTRUCT_PTR_ARRAY_PTR_END|TPTR_END)?;"
    )
    chunk_header_re = re.compile(r"\* Chunk: (\w+), versions: (\d+), strucTab: 0x[0-9A-Fa-f]+")
    version_re = re.compile(r"/\* Version: (\d+)[^*]*\*/")

    for line in lines:
        line = line.strip()
        if not line or line.startswith("//"):
            continue

        # Detect chunk
        chunk_match = chunk_header_re.search(line)
        if chunk_match:
            chunk_name, nb_versions = chunk_match.groups()
            current_chunk = {
                "name": chunk_name,
                "versions": []
            }
            schema["chunks"].append(current_chunk)
            continue

        # Detect version
        version_match = version_re.search(line)
        if version_match:
            version_id = int(version_match.group(1))
            current_version = {
                "version": version_id,
                "structs": []
            }
            current_chunk["versions"].append(current_version)
            continue

        # Struct start
        if struct_start_re.search(line):
            struct_stack.append({"members": []})
            continue

        # Struct end
        struct_end_match = struct_end_re.search(line)
        if struct_end_match:
            struct_name = struct_end_match.group(1)
            struct_data = struct_stack.pop()
            struct_data["name"] = struct_name

            if struct_stack:
                # Nested struct
                struct_stack[-1]["members"].append({
                    "name": struct_name,
                    "type": "struct",
                    "kind": "struct",
                    "members": struct_data["members"]
                })
            else:
                current_version["structs"].append(struct_data)
            continue

        # Struct member
        member_match = member_re.search(line)
        if member_match and struct_stack:
            start_macro, type_name, member_name, end_macro = member_match.groups()
            member_info = {"name": member_name, "type": type_name, "kind": "simple"}

            if start_macro:
                if start_macro == "TSTRUCT_ARRAY_PTR_START":
                    member_info["kind"] = "array"
                elif start_macro == "TSTRUCT_PTR_ARRAY_PTR_START":
                    member_info["kind"] = "ptr_array"
                elif start_macro == "TPTR_START":
                    member_info["kind"] = "ptr"

            struct_stack[-1]["members"].append(member_info)

    return schema

if __name__ == "__main__":
    txt_file = "output.txt"  # your 010 Editor dump
    schema_json = parse_txt_to_json(txt_file)
    with open("schema.json", "w", encoding="utf-8") as f:
        json.dump(schema_json, f, indent=2)
    print("Schema JSON saved to schema.json")
