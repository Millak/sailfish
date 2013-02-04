#include <cstdio>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <atomic>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>

#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>

#include "boost/timer/timer.hpp"

#include "tbb/parallel_for_each.h"
#include "tbb/parallel_for.h"
#include "tbb/task_scheduler_init.h"

#include "jellyfish/parse_dna.hpp"
#include "jellyfish/mapped_file.hpp"
#include "jellyfish/parse_read.hpp"
#include "jellyfish/sequence_parser.hpp"
#include "jellyfish/dna_codes.hpp"
#include "jellyfish/compacted_hash.hpp"
#include "jellyfish/mer_counting.hpp"
#include "jellyfish/misc.hpp"

#include "CountDBNew.hpp"
#include "cmph.h"

#include "PerfectHashIndex.hpp"

int mainCount( int argc, char *argv[] ) {

    using std::string;
    namespace po = boost::program_options;

    po::options_description generic("Command Line Options");
    generic.add_options()
    ("version,v", "print version string")
    ("help,h", "produce help message")
    ("index,i", po::value<string>(), "transcript index file [Sailfish format]")
    ("reads,r", po::value<std::vector<string>>()->multitoken(), "List of files containing reads")
    ("counts,c", po::value<string>(), "File where Sailfish read count is written")
    ;

    po::variables_map vm;

    try {

        po::store(po::command_line_parser(argc, argv).options(generic).run(), vm);

        if ( vm.count("help") ) {
            auto hstring = R"(
count
==========
Counts the kmers in the set of reads [reads] which also occur in
the Sailfish index [index].  The resulting set of counts relies on the
same index, and the counts will be written to the file [counts].
)";
            std::cout << hstring <<"\n";
            std::cout << generic << std::endl;
            std::exit(1);
        }
        po::notify(vm);

        string countsFile = vm["counts"].as<string>();

        auto phi = PerfectHashIndex::fromFile( vm["index"].as<string>() );
        std::cerr << "index contained " << phi.numKeys() << " kmers\n";

        size_t nkeys = phi.numKeys();
        size_t merLen = phi.kmerLength();

        size_t numActors = 12;
        std::vector<std::thread> threads;

        auto del = []( PerfectHashIndex* h ) -> void { /*do nothing*/; };
        auto phiPtr = std::shared_ptr<PerfectHashIndex>(&phi, del);
        CountDBNew thash( phiPtr );

        std::atomic<uint64_t> readNum{0};
        std::atomic<uint64_t> processedReads{0};

        std::vector<string> readFiles = vm["reads"].as<std::vector<string>>();
        for( auto rf : readFiles ) {
            std::cerr << "readFile: " << rf << ", ";
        }
        std::cerr << "\n";

        char** fnames = new char*[readFiles.size()];
        size_t z{0};
        size_t numFnames{0};
        for ( auto& s : readFiles ){
            // Ugly, yes?  But this is not as ugly as the alternatives.
            // The char*'s contained in fname are owned by the readFiles
            // vector and need not be manually freed.
            fnames[numFnames] = const_cast<char*>(s.c_str());
            ++numFnames;
        }

        CountDBNew rhash( phiPtr );

        // Open up the transcript file for reading
        // Create a jellyfish parser
        jellyfish::parse_read parser( fnames, fnames+numFnames, 1000);

        {
          boost::timer::auto_cpu_timer t;

          auto start = std::chrono::steady_clock::now();

          // Start the desired number of threads to parse the reads
          // and build our data structure.
          for (size_t k = 0; k < numActors; ++k) {

            threads.push_back( std::thread( 
                [&parser, &readNum, &rhash, &start, merLen]() -> void {
                    // Each thread gets it's own stream
                    jellyfish::parse_read::read_t* read;
                    jellyfish::parse_read::thread stream = parser.new_thread();
                    while ( (read = stream.next_read()) ) {

                        ++readNum;
                        if (readNum % 250000 == 0) {
                            auto end = std::chrono::steady_clock::now();
                            auto sec = std::chrono::duration_cast<std::chrono::seconds>(end-start);
                            auto nsec = sec.count();
                            auto rate = (nsec > 0) ? readNum / sec.count() : 0;
                            std::cerr << "processed " << readNum << " reads (" << rate << ") reads/s\r";
                        }

                        std::string seq = std::string(read->seq_s, std::distance(read->seq_s, read->seq_e) - 1 );
                        auto newEnd  = std::remove( seq.begin(), seq.end(), '\n' );
                        auto readLen = std::distance( seq.begin(), newEnd );
                        size_t numKmers = readLen - merLen + 1;
                        size_t offset = 0;
                        while ( offset < numKmers ) {
                            auto mer = seq.substr( offset, merLen );
                            auto binMer = jellyfish::parse_dna::mer_string_to_binary( mer.c_str(), merLen );
                            auto rmer = jellyfish::parse_dna::reverse_complement( binMer, merLen );
                            
                            binMer = (binMer < rmer) ? binMer : rmer;
                            rhash.inc(binMer);
                            ++offset;
                        }
                    }

                }) 
            );

        }

        // Wait for all of the threads to finish
        for ( auto& thread : threads ){ thread.join(); }
        std::cerr << "\n" << std::endl;
        rhash.dumpCountsToFile(countsFile);

        }

    } catch (po::error &e) {
        std::cerr << "exception : [" << e.what() << "]. Exiting.\n";
        std::exit(1);
    }
}