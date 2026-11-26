file(READ "${INPUT}" data HEX)
string(REGEX REPLACE "(..)" "\\\\x\\1" data "${data}")
file(WRITE "${OUTPUT}" "#include <cstddef>\nnamespace ir {\nextern const unsigned char wordnet_data[] = \"${data}\";\nextern const std::size_t wordnet_size = sizeof(wordnet_data) - 1;\n}\n")
