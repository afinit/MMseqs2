#include "Debug.h"
#include "Parameters.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Matcher.h"
#include "Util.h"
#include "itoa.h"

#include "Orf.h"

#include <unistd.h>
#include <climits>
#include <algorithm>

#ifdef OPENMP
#include <omp.h>
#endif

const char newline = '\n';

unsigned int getFrames(std::string frames) {
    unsigned int result = 0;

    std::vector<std::string> frame = Util::split(frames, ",");

    if(std::find(frame.begin(), frame.end(), "1") != frame.end()) {
        result |= Orf::FRAME_1;
    }

    if(std::find(frame.begin(), frame.end(), "2") != frame.end()) {
        result |= Orf::FRAME_2;
    }

    if(std::find(frame.begin(), frame.end(), "3") != frame.end()) {
        result |= Orf::FRAME_3;
    }

    return result;
}

int extractorfs(int argc, const char **argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 2);

#ifdef OPENMP
    omp_set_num_threads(par.threads);
#endif

    DBReader<unsigned int> reader(par.db1.c_str(), par.db1Index.c_str());
    reader.open(DBReader<unsigned int>::NOSORT);

    DBReader<unsigned int> headerReader(par.hdr1.c_str(), par.hdr1Index.c_str());
    headerReader.open(DBReader<unsigned int>::NOSORT);

    DBWriter sequenceWriter(par.db2.c_str(), par.db2Index.c_str(), par.threads);
    sequenceWriter.open();

    DBWriter headerWriter(par.hdr2.c_str(), par.hdr2Index.c_str(), par.threads);
    headerWriter.open();


    unsigned int forwardFrames = getFrames(par.forwardFrames);
    unsigned int reverseFrames = getFrames(par.reverseFrames);

#pragma omp parallel
    {
        Orf orf(par.translationTable, par.useAllTableStarts);
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif
        size_t querySize = 0;
        size_t queryFrom = 0;
        Util::decomposeDomainByAminoAcid(reader.getAminoAcidDBSize(), reader.getSeqLens(), reader.getSize(),
                                         thread_idx, par.threads, &queryFrom, &querySize);
        if (querySize == 0) {
            queryFrom = 0;
        }

        std::vector<Orf::SequenceLocation> res;
        res.reserve(1000);
        for (unsigned int i = queryFrom; i < (queryFrom + querySize); ++i){
            Debug::printProgress(i);

            unsigned int key = reader.getDbKey(i);
            const char* data = reader.getData(i);
            size_t dataLength = reader.getSeqLens(i);

            if(!orf.setSequence(data, dataLength - 2)) {
                Debug(Debug::WARNING) << "Invalid sequence with index " << i << "!\n";
                continue;
            }

            const char* header = headerReader.getData(i);
            size_t headerLength = headerReader.getSeqLens(i);

            orf.findAll(res, par.orfMinLength, par.orfMaxLength, par.orfMaxGaps, forwardFrames, reverseFrames, par.orfStartMode);
            for (std::vector<Orf::SequenceLocation>::const_iterator it = res.begin(); it != res.end(); ++it) {
                Orf::SequenceLocation loc = *it;

                if (par.contigStartMode < 2 && (loc.hasIncompleteStart == par.contigStartMode)) {
                    continue;
                }
                if (par.contigEndMode   < 2 && (loc.hasIncompleteEnd   == par.contigEndMode)) {
                    continue;
                }

                char buffer[LINE_MAX];
                snprintf(buffer, LINE_MAX, "%.*s [Orf: %d, %zu, %zu, %d, %d, %d]\n", (unsigned int)(headerLength - 2), header, key, loc.from, loc.to, loc.strand, loc.hasIncompleteStart, loc.hasIncompleteEnd);

                headerWriter.writeData(buffer, strlen(buffer), key, thread_idx);

                sequenceWriter.writeStart(thread_idx);
                std::pair<const char*, size_t> sequence = orf.getSequence(loc);
                sequenceWriter.writeAdd(sequence.first, sequence.second, thread_idx);
                sequenceWriter.writeAdd(&newline, 1, thread_idx);
                sequenceWriter.writeEnd(key, thread_idx);
            }
            res.clear();
        }
    }
    headerWriter.close();
    sequenceWriter.close(Sequence::NUCLEOTIDES);
    headerReader.close();
    reader.close();

    // make identifiers stable
#pragma omp parallel
    {
#pragma omp single
        {
#pragma omp task
            {
                DBReader<unsigned int> orfHeaderReader(par.hdr2.c_str(), par.hdr2Index.c_str(),
                                                       DBReader<unsigned int>::USE_INDEX);
                orfHeaderReader.open(DBReader<unsigned int>::SORT_BY_ID_OFFSET);
                FILE *hIndex = fopen((par.hdr2Index + "_tmp").c_str(), "w");
                if (hIndex == NULL) {
                    Debug(Debug::ERROR) << "Could not open " << par.hdr2Index << "_tmp for writing!\n";
                    EXIT(EXIT_FAILURE);
                }
                for (size_t i = 0; i < orfHeaderReader.getSize(); i++) {
                    DBReader<unsigned int>::Index *idx = orfHeaderReader.getIndex(i);
                    char buffer[1024];
                    size_t len = DBWriter::indexToBuffer(buffer, i, idx->offset, orfHeaderReader.getSeqLens(i));
                    int written = fwrite(buffer, sizeof(char), len, hIndex);
                    if (written != (int) len) {
                        Debug(Debug::ERROR) << "Could not write to data file " << par.hdr2Index << "_tmp\n";
                        EXIT(EXIT_FAILURE);
                    }
                }
                fclose(hIndex);
                orfHeaderReader.close();
                std::rename((par.hdr2Index + "_tmp").c_str(), par.hdr2Index.c_str());
            }

#pragma omp task
            {
                DBReader<unsigned int> orfSequenceReader(par.db2.c_str(), par.db2Index.c_str(),
                                                         DBReader<unsigned int>::USE_INDEX);
                orfSequenceReader.open(DBReader<unsigned int>::SORT_BY_ID_OFFSET);

                FILE *sIndex = fopen((par.db2Index + "_tmp").c_str(), "w");
                if (sIndex == NULL) {
                    Debug(Debug::ERROR) << "Could not open " << par.db2Index << "_tmp for writing!\n";
                    EXIT(EXIT_FAILURE);
                }

                for (size_t i = 0; i < orfSequenceReader.getSize(); i++) {
                    DBReader<unsigned int>::Index *idx = (orfSequenceReader.getIndex(i));
                    char buffer[1024];
                    size_t len = DBWriter::indexToBuffer(buffer, i, idx->offset, orfSequenceReader.getSeqLens(i));
                    int written = fwrite(buffer, sizeof(char), len, sIndex);
                    if (written != (int) len) {
                        Debug(Debug::ERROR) << "Could not write to data file " << par.db2Index << "_tmp\n";
                        EXIT(EXIT_FAILURE);
                    }
                }
                fclose(sIndex);
                orfSequenceReader.close();
                std::rename((par.db2Index + "_tmp").c_str(), par.db2Index.c_str());
            }
        }
    }
    return EXIT_SUCCESS;
}

