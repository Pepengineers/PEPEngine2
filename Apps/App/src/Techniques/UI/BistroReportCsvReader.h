#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace BistroReport
{
// Read the quoted fields emitted by BistroBenchmarkData::Csv, including embedded newlines.
class CsvReader
{
  public:
    explicit CsvReader(const std::filesystem::path &path) : _input(path)
    {
        if (Next())
            for (size_t i = 0; i < _fields.size(); ++i) _columns.emplace(_fields[i], i);
    }
    std::streamoff Position() { return _input.tellg(); }
    void Seek(std::streamoff offset) { _input.seekg(offset); }
    bool Next(bool skipSamples = false)
    {
        _fields.clear();
        std::string line;
        do
        {
            if (!std::getline(_input, line)) return false;
            // Quoted samples can span lines. Consume those with the normal CSV parser.
        } while (skipSamples && (line.rfind("frame,", 0) == 0 || line.rfind("interval,", 0) == 0) &&
            line.find('"') == line.npos);
        std::string field;
        bool quoted = false;
        for (;;)
        {
            for (size_t i = 0; i < line.size(); ++i)
            {
                char ch = line[i];
                if (ch == '"')
                {
                    if (quoted && i + 1 < line.size() && line[i + 1] == '"')
                    {
                        field += ch;
                        ++i;
                    }
                    else
                        quoted = !quoted;
                }
                else if (ch == ',' && !quoted)
                {
                    _fields.push_back(std::move(field));
                    field.clear();
                }
                else
                    field += ch;
            }
            if (!quoted) break;
            field += '\n';
            if (!std::getline(_input, line)) return false;
        }
        _fields.push_back(std::move(field));
        return true;
    }
    const std::string &Get(const char *key) const
    {
        static const std::string empty;
        const auto it = _columns.find(key);
        return it != _columns.end() && it->second < _fields.size() ? _fields[it->second] : empty;
    }

  private:
    std::ifstream _input;
    std::map<std::string, size_t, std::less<>> _columns;
    std::vector<std::string> _fields;
};
}
