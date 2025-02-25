/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <iostream>
#include <string>

#include "test_harness/debug_utils.h"
#include "test_harness/test.h"

class poc_test : public test_harness::test {
    public:
    poc_test(const std::string &config, int64_t trace_level) : test(config)
    {
        test_harness::_trace_level = trace_level;
    }

    void
    run()
    {
        test::run();
    }
};

const std::string poc_test::test::name = "poc_test";
const std::string poc_test::test::default_config =
  "enable_tracking=true,collection_count=2,key_count=5,value_size=20,"
  "read_threads=1,duration_seconds=1";

int
main(int argc, char *argv[])
{
    std::string cfg = "";
    int64_t trace_level = 0;
    int64_t error_code = 0;

    // Parse args
    // -C   : Configuration
    // -t   : Trace level
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-C") {
            if ((i + 1) < argc)
                cfg = argv[++i];
            else {
                std::cerr << "No value given for option " << argv[i] << std::endl;
                return (-1);
            }
        } else if (std::string(argv[i]) == "-t") {
            if ((i + 1) < argc)
                trace_level = std::stoi(argv[++i]);
            else {
                std::cerr << "No value given for option " << argv[i] << std::endl;
                return (-1);
            }
        }
    }

    // Check if default configuration should be used
    if (cfg.empty())
        cfg = poc_test::test::default_config;

    std::cout << "Configuration\t:" << cfg << std::endl;
    std::cout << "Trace level\t:" << trace_level << std::endl;

    poc_test(cfg, trace_level).run();
    return (0);
}
