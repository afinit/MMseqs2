#include "Prefiltering.h"
#include "NucleotideMatrix.h"
#include "ReducedMatrix.h"
#include "ExtendedSubstitutionMatrix.h"
#include "SubstitutionMatrixProfileStates.h"
#include "PatternCompiler.h"
#include "FileUtil.h"
#include "IndexBuilder.h"
#include "Timer.h"

namespace prefilter {
#include "ExpOpt3_8_polished.cs32.lib.h"
}

#ifdef OPENMP
#include <omp.h>
#endif

Prefiltering::Prefiltering(const std::string &targetDB,
                           const std::string &targetDBIndex,
                           int querySeqType, int targetSeqType_,
                           const Parameters &par) :
        targetDB(targetDB),
        targetDBIndex(targetDBIndex),
        _2merSubMatrix(NULL),
        _3merSubMatrix(NULL),
        splits(par.split),
        kmerSize(par.kmerSize),
        spacedKmerPattern(par.spacedKmerPattern),
        spacedKmer(par.spacedKmer != 0),
        alphabetSize(par.alphabetSize),
        maskMode(par.maskMode),
        splitMode(par.splitMode),
        scoringMatrixFile(par.scoringMatrixFile),
        targetSeqType(targetSeqType_),
        maxResListLen(par.maxResListLen),
        kmerScore(par.kmerScore),
        sensitivity(par.sensitivity),
        resListOffset(par.resListOffset),
        maxSeqLen(par.maxSeqLen),
        querySeqType(querySeqType),
        diagonalScoring(par.diagonalScoring != 0),
        minDiagScoreThr(static_cast<unsigned int>(par.minDiagScoreThr)),
        aaBiasCorrection(par.compBiasCorrection != 0),
        covThr(par.covThr), covMode(par.covMode), includeIdentical(par.includeIdentity),
        preloadMode(par.preloadMode),
        threads(static_cast<unsigned int>(par.threads)) {
#ifdef OPENMP
    Debug(Debug::INFO) << "Using " << threads << " threads.\n";
#endif

    int indexMasked = maskMode;
    int minKmerThr = INT_MIN;
    std::string indexDB = PrefilteringIndexReader::searchForIndex(targetDB);
    if (indexDB != "") {
        Debug(Debug::INFO) << "Use index  " << indexDB << "\n";

        int dataMode = DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA;
        if(preloadMode == Parameters::PRELOAD_MODE_AUTO){
            if(sensitivity > 6.0){
                preloadMode = Parameters::PRELOAD_MODE_FREAD;
            }else{
                preloadMode = Parameters::PRELOAD_MODE_MMAP_TOUCH;
            }
        }
        if (preloadMode == Parameters::PRELOAD_MODE_FREAD) {
            dataMode |= DBReader<unsigned int>::USE_FREAD;
        }
        tdbr = new DBReader<unsigned int>(indexDB.c_str(), (indexDB + ".index").c_str(), dataMode);
        tdbr->open(DBReader<unsigned int>::NOSORT);

        templateDBIsIndex = PrefilteringIndexReader::checkIfIndexFile(tdbr);
        if (templateDBIsIndex == true) {
            // exchange reader with old reader
            tidxdbr = tdbr;
            bool touch = false;
            if (preloadMode == Parameters::PRELOAD_MODE_MMAP_TOUCH) {
                touch = true;
            }
            tdbr = PrefilteringIndexReader::openNewReader(tdbr, touch);
            PrefilteringIndexReader::printSummary(tidxdbr);
            PrefilteringIndexData data = PrefilteringIndexReader::getMetadata(tidxdbr);
            kmerSize = data.kmerSize;
            alphabetSize = data.alphabetSize;
            targetSeqType = data.seqType;
            spacedKmer   = (data.spacedKmer == 1) ? true : false;
            indexMasked = data.mask;

            if (querySeqType == Sequence::HMM_PROFILE && targetSeqType == Sequence::HMM_PROFILE) {
                Debug(Debug::ERROR) << "Query-profiles cannot be searched against a target-profile database!\n";
                EXIT(EXIT_FAILURE);
            }

            splits = 1;
            spacedKmer = data.spacedKmer != 0;
            minKmerThr = data.kmerThr;
            scoringMatrixFile = PrefilteringIndexReader::getSubstitutionMatrixName(tidxdbr);
        } else {
            Debug(Debug::ERROR) << "Outdated index version. Please recompute it with 'createindex'!\n";
            EXIT(EXIT_FAILURE);
        }
    } else {
        Debug(Debug::INFO) << "Could not find precomputed index. Compute index.\n";
        tdbr = new DBReader<unsigned int>(targetDB.c_str(), targetDBIndex.c_str());
        tdbr->open(DBReader<unsigned int>::NOSORT);

        if (par.preloadMode != Parameters::PRELOAD_MODE_MMAP) {
            tdbr->readMmapedDataInMemory();
            tdbr->mlock();
        }

        templateDBIsIndex = false;
    }

    // init the substitution matrices
    switch (querySeqType) {
        case Sequence::NUCLEOTIDES:
            subMat = new NucleotideMatrix(scoringMatrixFile.c_str(), 1.0, 0.0);
            alphabetSize = subMat->alphabetSize;
            break;
        case Sequence::AMINO_ACIDS:
            subMat = getSubstitutionMatrix(scoringMatrixFile, alphabetSize, 8.0, false);
            alphabetSize = subMat->alphabetSize;
            break;
        case Sequence::HMM_PROFILE:
            // needed for Background distributions
            subMat = getSubstitutionMatrix(scoringMatrixFile, alphabetSize, 8.0, false);
            break;
        case Sequence::PROFILE_STATE_PROFILE:
            subMat = getSubstitutionMatrix(scoringMatrixFile, alphabetSize, 8.0, true);
            alphabetSize = subMat->alphabetSize;
            break;
        default:
            Debug(Debug::ERROR) << "Query sequence type not implemented!\n";
            EXIT(EXIT_FAILURE);
    }

    // investigate if it makes sense to mask the profile consensus sequence
    if (targetSeqType == Sequence::HMM_PROFILE || targetSeqType == Sequence::PROFILE_STATE_SEQ) {
        maskMode = 0;
    }

    takeOnlyBestKmer = (par.exactKmerMatching==1)||
                       (targetSeqType == Sequence::HMM_PROFILE && querySeqType == Sequence::AMINO_ACIDS) ||
                       (targetSeqType == Sequence::NUCLEOTIDES && querySeqType == Sequence::NUCLEOTIDES);

    int originalSplits = splits;
    size_t memoryLimit;
    if (par.splitMemoryLimit > 0) {
        memoryLimit = static_cast<size_t>(par.splitMemoryLimit) * 1024;
    } else {
        memoryLimit = static_cast<size_t>(Util::getTotalSystemMemory() * 0.9);
    }
    setupSplit(*tdbr, alphabetSize - 1, querySeqType,
               threads, templateDBIsIndex, maxResListLen,
               memoryLimit, &kmerSize, &splits, &splitMode);

    if(targetSeqType != Sequence::NUCLEOTIDES){
        kmerThr = getKmerThreshold(sensitivity, querySeqType, kmerScore, kmerSize);
    }
    if (templateDBIsIndex == true) {
        if (splits != originalSplits) {
            Debug(Debug::WARNING) << "Required split count does not match index table split count. Recomputing index table!\n";
            reopenTargetDb();
        } else if (kmerThr < minKmerThr) {
            Debug(Debug::WARNING) << "Required k-mer threshold ( " << kmerThr
                                  << ") does not match index table k-mer threshold (" << minKmerThr << "). "
                                  << "Recomputing index table!\n";
            reopenTargetDb();
        } else if ((querySeqType == Sequence::HMM_PROFILE || querySeqType == Sequence::PROFILE_STATE_PROFILE) && minKmerThr != 0) {
            Debug(Debug::WARNING) << "Query profiles require an index table k-mer threshold of 0. Recomputing index table!\n";
            reopenTargetDb();
        } else if (indexMasked != maskMode) {
            Debug(Debug::WARNING) << "Can not use masked index for unmasked prefiltering. Recomputing index table!\n";
            reopenTargetDb();
        }
    }

    Debug(Debug::INFO) << "Target database: " << targetDB << "(Size: " << tdbr->getSize() << ")\n";

    if (splitMode == Parameters::QUERY_DB_SPLIT) {
        // create the whole index table
        getIndexTable(0, 0, tdbr->getSize());
    } else if (splitMode == Parameters::TARGET_DB_SPLIT) {
        sequenceLookup = NULL;
        indexTable = NULL;
    } else {
        Debug(Debug::ERROR) << "Invalid split mode: " << splitMode << "\n";
        EXIT(EXIT_FAILURE);
    }

    Debug(Debug::INFO) << "Query database type: " << DBReader<unsigned int>::getDbTypeName(querySeqType) << "\n";
    Debug(Debug::INFO) << "Target database type: " << DBReader<unsigned int>::getDbTypeName(targetSeqType) << "\n";
}

Prefiltering::~Prefiltering() {
    if (indexTable != NULL) {
        delete indexTable;
    }

    if (sequenceLookup != NULL) {
        delete sequenceLookup;
    }

    tdbr->close();
    delete tdbr;

    if (templateDBIsIndex == true) {
        tidxdbr->close();
        delete tidxdbr;
    }

    delete subMat;

    if (_2merSubMatrix != NULL && templateDBIsIndex == false) {
        ScoreMatrix::cleanup(_2merSubMatrix);
    }
    if (_3merSubMatrix != NULL && templateDBIsIndex == false) {
        ScoreMatrix::cleanup(_3merSubMatrix);
    }
}

void Prefiltering::reopenTargetDb() {
    if (templateDBIsIndex == true) {
        tidxdbr->close();
        delete tidxdbr;
        tidxdbr = NULL;
    }

    tdbr->close();
    delete tdbr;

    Debug(Debug::INFO) << "Index table not compatible with chosen settings. Compute index.\n";
    tdbr = new DBReader<unsigned int>(targetDB.c_str(), targetDBIndex.c_str());
    tdbr->open(DBReader<unsigned int>::NOSORT);

    if (preloadMode != Parameters::PRELOAD_MODE_MMAP) {
        tdbr->readMmapedDataInMemory();
        tdbr->mlock();
    }

    templateDBIsIndex = false;
}

void Prefiltering::setupSplit(DBReader<unsigned int>& dbr, const int alphabetSize, const unsigned int querySeqTyp, const int threads,
                              const bool templateDBIsIndex, const size_t maxResListLen, const size_t memoryLimit,
                              int *kmerSize, int *split, int *splitMode) {
    size_t neededSize = estimateMemoryConsumption(1,
                                                  dbr.getSize(), dbr.getAminoAcidDBSize(),  maxResListLen, alphabetSize,
                                                  *kmerSize == 0 ? // if auto detect kmerSize
                                                  IndexTable::computeKmerSize(dbr.getAminoAcidDBSize()) : *kmerSize, querySeqTyp,
                                                  threads);
    if (neededSize > 0.9 * memoryLimit) {
        // memory is not enough to compute everything at once
        //TODO add PROFILE_STATE (just 6-mers)
        std::pair<int, int> splitSettings = Prefiltering::optimizeSplit(memoryLimit, &dbr,
                                                                        alphabetSize, *kmerSize, querySeqTyp, threads);
        if (splitSettings.second == -1) {
            Debug(Debug::ERROR) << "Can not fit databased into " << memoryLimit
                                << " byte. Please use a computer with more main memory.\n";
            EXIT(EXIT_FAILURE);
        }
        if (*kmerSize == 0) {
            // set k-mer based on aa size in database
            // if we have less than 10Mio * 335 amino acids use 6mers
            *kmerSize = splitSettings.first;
        }

        if (*split == Parameters::AUTO_SPLIT_DETECTION) {
            *split = splitSettings.second;
        }

        if (*splitMode == Parameters::DETECT_BEST_DB_SPLIT) {
            *splitMode = Parameters::TARGET_DB_SPLIT;
        }
    } else { // memory is  enough to compute everything with split setting
        if (*kmerSize == 0) {
            const int tmpSplit = (*split > 1) ? *split : 1;
            size_t aaSize = dbr.getAminoAcidDBSize() / tmpSplit;
            *kmerSize = IndexTable::computeKmerSize(aaSize);
        }

        if (*split == Parameters::AUTO_SPLIT_DETECTION) {
            *split = 1;
        }

        if (*splitMode == Parameters::DETECT_BEST_DB_SPLIT) {
            if (templateDBIsIndex == true && *split > 1) {
                *splitMode = Parameters::TARGET_DB_SPLIT;
            } else {
#ifdef HAVE_MPI
                *splitMode = Parameters::QUERY_DB_SPLIT;
#else
                *splitMode = Parameters::TARGET_DB_SPLIT;
#endif
            }
        }
    }

    Debug(Debug::INFO) << "Use kmer size " << *kmerSize << " and split "
                       << *split << " using " << Parameters::getSplitModeName(*splitMode) << " split mode.\n";
    neededSize = estimateMemoryConsumption((*splitMode == Parameters::TARGET_DB_SPLIT) ? *split : 1, dbr.getSize(),
                                           dbr.getAminoAcidDBSize(), maxResListLen, alphabetSize, *kmerSize, querySeqTyp, threads);
    Debug(Debug::INFO) << "Needed memory (" << neededSize << " byte) of total memory (" << memoryLimit
                       << " byte)\n";
    if (neededSize > 0.9 * memoryLimit) {
        Debug(Debug::WARNING) << "WARNING: MMseqs processes needs more main memory than available."
                "Increase the size of --split or set it to 0 to automatically optimize target database split.\n";
        if (templateDBIsIndex == true) {
            Debug(Debug::WARNING) << "WARNING: Split has to be computed by createindex if precomputed index is used.\n";
        }
    }
}

void Prefiltering::mergeOutput(const std::string &outDB, const std::string &outDBIndex,
                               const std::vector<std::pair<std::string, std::string>> &filenames) {
    Timer timer;
    if (filenames.size() < 2) {
        std::rename(filenames[0].first.c_str(), outDB.c_str());
        std::rename(filenames[0].second.c_str(), outDBIndex.c_str());
        Debug(Debug::INFO) << "No merging needed.\n";
        return;
    }
    std::list<std::pair<std::string, std::string>> files(filenames.begin(), filenames.end());

    const std::pair<std::string, std::string> out = std::make_pair((outDB + "_merged" ),
                                                                   (outDBIndex + "_merged"));


    DBWriter writer(out.first.c_str(), out.second.c_str(), 1);
    writer.open(1024 * 1024 * 1024); // 1 GB buffer
    writer.mergeFilePair(filenames);
    writer.close();
    for(size_t i = 0; i < filenames.size(); i++){
        // remove split
        int error = remove(filenames[i].first.c_str());
        if(error != 0){
            Debug(Debug::ERROR) << "Error while deleting " << filenames[i].first << " in mergeOutput!\n";
            EXIT(EXIT_FAILURE);
        }
        error = remove(filenames[i].second.c_str());
        if(error != 0){
            Debug(Debug::ERROR) << "Error while deleting " << filenames[i].second << " in mergeOutput!\n";
            EXIT(EXIT_FAILURE);
        }
    }
    // sort merged entries by evalue
    DBReader<unsigned int> dbr(out.first.c_str(), out.second.c_str());
    dbr.open(DBReader<unsigned int>::LINEAR_ACCCESS);
    DBWriter dbw(outDB.c_str(), outDBIndex.c_str(), threads);
    dbw.open(1024 * 1024 * 1024);
#pragma omp parallel
    {
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif

        std::string result;
        result.reserve(BUFFER_SIZE);
        char buffer[100];
#pragma omp  for schedule(dynamic, 10)
        for (size_t id = 0; id < dbr.getSize(); id++) {
            unsigned int dbKey = dbr.getDbKey(id);
            char *data = dbr.getData(id);
            std::vector<hit_t> hits = QueryMatcher::parsePrefilterHits(data);
            if (hits.size() > 1) {
                std::sort(hits.begin(), hits.end(), hit_t::compareHitsByPValueAndId);
            }
            for(size_t hit_id = 0; hit_id < hits.size(); hit_id++){
                int len = QueryMatcher::prefilterHitToBuffer(buffer, hits[hit_id]);
                result.append(buffer, len);
            }
            dbw.writeData(result.c_str(), result.size(), dbKey, thread_idx);
            result.clear();
        }
    }
    Debug(Debug::INFO) << out.first << " " << out.second << "\n";
    dbw.close();
    dbr.close();
    int error = remove(out.first.c_str());
    if(error != 0){
        Debug(Debug::ERROR) << "Error while deleting " << out.first << " in mergeOutput!\n";
        EXIT(EXIT_FAILURE);
    }
    error = remove(out.second.c_str());
    if(error != 0){
        Debug(Debug::ERROR) << "Error while deleting " << out.second << " in mergeOutput!\n";
        EXIT(EXIT_FAILURE);
    }

    Debug(Debug::INFO) << "\nTime for merging results: " << timer.lap() << "\n";
}


ScoreMatrix *Prefiltering::getScoreMatrix(const BaseMatrix& matrix, const size_t kmerSize) {
    // profile only uses the 2mer, 3mer matrix
    if (targetSeqType == Sequence::HMM_PROFILE || targetSeqType == Sequence::PROFILE_STATE_SEQ) {
        return NULL;
    }

    ScoreMatrix *result = NULL;
    if (templateDBIsIndex == true) {
        switch(kmerSize) {
            case 2:
                result = PrefilteringIndexReader::get2MerScoreMatrix(tidxdbr, false);
                break;
            case 3:
                result = PrefilteringIndexReader::get3MerScoreMatrix(tidxdbr, false);
                break;
            default:
                break;
        }
    }

    if (result != NULL) {
        return result;
    }
    return ExtendedSubstitutionMatrix::calcScoreMatrix(matrix, kmerSize);

}

// TODO reimplement split index feature
void Prefiltering::getIndexTable(int /*split*/, size_t dbFrom, size_t dbSize) {
    if (templateDBIsIndex == true) {
        indexTable = PrefilteringIndexReader::generateIndexTable(tidxdbr, false);

        if (maskMode == 0) {
            sequenceLookup = PrefilteringIndexReader::getUnmaskedSequenceLookup(tidxdbr, false);
        } else if (maskMode == 1) {
            sequenceLookup = PrefilteringIndexReader::getMaskedSequenceLookup(tidxdbr, false);
        }
    } else {
        Timer timer;

        Sequence tseq(maxSeqLen, targetSeqType, subMat, kmerSize, spacedKmer, aaBiasCorrection, true, spacedKmerPattern);
        int localKmerThr = (querySeqType == Sequence::HMM_PROFILE ||
                            querySeqType == Sequence::PROFILE_STATE_PROFILE ||
                            querySeqType == Sequence::NUCLEOTIDES ||
                            (targetSeqType != Sequence::HMM_PROFILE && takeOnlyBestKmer == true) ) ? 0 : kmerThr;

        // remove X or N for seeding
        int adjustAlphabetSize = (targetSeqType == Sequence::NUCLEOTIDES || targetSeqType == Sequence::AMINO_ACIDS)
                           ? alphabetSize -1 : alphabetSize;
        indexTable = new IndexTable(adjustAlphabetSize, kmerSize, false);
        SequenceLookup **maskedLookup   = maskMode == 1 ? &sequenceLookup : NULL;
        SequenceLookup **unmaskedLookup = maskMode == 0 ? &sequenceLookup : NULL;

        Debug(Debug::INFO) << "Index table k-mer threshold: " << localKmerThr << "\n";
        IndexBuilder::fillDatabase(indexTable, maskedLookup, unmaskedLookup, *subMat,  &tseq, tdbr, dbFrom, dbFrom + dbSize, localKmerThr);

        if (diagonalScoring == false) {
            delete sequenceLookup;
            sequenceLookup = NULL;
        }

        indexTable->printStatistics(subMat->int2aa);
        tdbr->remapData();
        Debug(Debug::INFO) << "Time for index table init: " << timer.lap() << "\n";
    }

    // init the substitution matrices
    switch (querySeqType) {
        case Sequence::AMINO_ACIDS:
            // Do not add X
            subMat->alphabetSize = subMat->alphabetSize - 1;
            _2merSubMatrix = getScoreMatrix(*subMat, 2);
            _3merSubMatrix = getScoreMatrix(*subMat, 3);
            subMat->alphabetSize = alphabetSize;
            break;
        case Sequence::HMM_PROFILE:
        case Sequence::PROFILE_STATE_PROFILE:
        case Sequence::NUCLEOTIDES:
        default:
            if (_2merSubMatrix != NULL && templateDBIsIndex == false) {
                delete _2merSubMatrix;
            }

            if (_3merSubMatrix != NULL && templateDBIsIndex == false) {
                delete _3merSubMatrix;
            }
            _2merSubMatrix = NULL;
            _3merSubMatrix = NULL;
            break;
    }
}

bool Prefiltering::isSameQTDB(const std::string &queryDB) {
    //  check if when qdb and tdb have the same name an index extension exists
    std::string check(targetDB);
    size_t pos = check.find(queryDB);
    int match = false;
    if (pos == 0) {
        check.replace(0, queryDB.length(), "");
        PatternCompiler regex("^\\.s?k[5-7]$");
        match = regex.isMatch(check.c_str());
    }
    // if no match found or two matches found (we want exactly one match)
    return (queryDB.compare(targetDB) == 0 || (match == true));
}

void Prefiltering::runAllSplits(const std::string &queryDB, const std::string &queryDBIndex,
                                const std::string &resultDB, const std::string &resultDBIndex) {
    runSplits(queryDB, queryDBIndex, resultDB, resultDBIndex, 0, splits);
}

#ifdef HAVE_MPI
void Prefiltering::runMpiSplits(const std::string &queryDB, const std::string &queryDBIndex,
                                const std::string &resultDB, const std::string &resultDBIndex) {

    splits = std::max(MMseqsMPI::numProc, splits);
    size_t fromSplit = 0;
    size_t splitCount = 1;
    // if split size is great than nodes than we have to
    // distribute all splits equally over all nodes
    unsigned int * splitCntPerProc = new unsigned int[MMseqsMPI::numProc];
    memset(splitCntPerProc, 0, sizeof(unsigned int) * MMseqsMPI::numProc);
    for(int i = 0; i < splits; i++){
        splitCntPerProc[i % MMseqsMPI::numProc] += 1;
    }
    for(int i = 0; i < MMseqsMPI::rank; i++){
        fromSplit += splitCntPerProc[i];
    }

    splitCount = splitCntPerProc[MMseqsMPI::rank];
    delete[] splitCntPerProc;

    std::pair<std::string, std::string> result = Util::createTmpFileNames(resultDB, resultDBIndex, MMseqsMPI::rank);
    int hasResult = runSplits(queryDB, queryDBIndex, result.first, result.second, fromSplit, splitCount) == true ? 1 : 0;

    int *results = NULL;
    if (MMseqsMPI::isMaster()) {
        results = new int[MMseqsMPI::numProc]();
    }

    MPI_Gather(&hasResult, 1, MPI_INT, results, 1, MPI_INT, MMseqsMPI::MASTER, MPI_COMM_WORLD);
    if (MMseqsMPI::isMaster()) {
        // gather does not write the result of the master into the array
        results[MMseqsMPI::MASTER] = hasResult;

        std::vector<std::pair<std::string, std::string>> splitFiles;
        for (int i = 0; i < MMseqsMPI::numProc; ++i) {
            if (results[i] == 1) {
                splitFiles.push_back(Util::createTmpFileNames(resultDB, resultDBIndex, i));
            }
        }

        if (splitFiles.size() > 0) {
            // merge output ffindex databases
            mergeFiles(resultDB, resultDBIndex, splitFiles);
        } else {
            Debug(Debug::ERROR) << "Aborting. No results were computed!\n";
            EXIT(EXIT_FAILURE);
        }

        delete results;
    }

}
#endif

bool Prefiltering::runSplits(const std::string &queryDB, const std::string &queryDBIndex,
                             const std::string &resultDB, const std::string &resultDBIndex,
                             size_t fromSplit, size_t splitProcessCount) {
    bool sameQTDB = isSameQTDB(queryDB);
    DBReader<unsigned int> *qdbr;
    if (templateDBIsIndex == false && sameQTDB == true) {
        qdbr = tdbr;
    } else {
        qdbr = new DBReader<unsigned int>(queryDB.c_str(), queryDBIndex.c_str());
        qdbr->open(DBReader<unsigned int>::LINEAR_ACCCESS);
    }
    Debug(Debug::INFO) << "Query database: " << queryDB << "(size=" << qdbr->getSize() << ")\n";

    size_t freeSpace =  FileUtil::getFreeSpace(FileUtil::dirName(resultDB).c_str());
    size_t estimatedHDDMemory = estimateHDDMemoryConsumption(qdbr->getSize(), maxResListLen);
    if (freeSpace < estimatedHDDMemory){
        Debug(Debug::WARNING) << "Warning: Hard disk might not have enough free space (" << freeSpace << " bytes left)."
                            << "The prefilter result might need maximal " << estimatedHDDMemory << " bytes.\n";
//        EXIT(EXIT_FAILURE);
    }

    size_t dbSize = 0;
    if (splitMode == Parameters::TARGET_DB_SPLIT) {
        dbSize = tdbr->getSize();
    } else if (splitMode == Parameters::QUERY_DB_SPLIT) {
        dbSize = qdbr->getSize();
    }

    bool hasResult = false;
    size_t totalSplits = std::min(dbSize, (size_t) splits);
    if (splitProcessCount > 1) {
        // splits template database into x sequence steps
        std::vector<std::pair<std::string, std::string> > splitFiles;
        for (size_t i = fromSplit; i < (fromSplit + splitProcessCount) && i < totalSplits; i++) {
            std::pair<std::string, std::string> filenamePair = Util::createTmpFileNames(resultDB, resultDBIndex, i);
            if (runSplit(qdbr, filenamePair.first.c_str(), filenamePair.second.c_str(), i, totalSplits, sameQTDB)) {
                splitFiles.push_back(filenamePair);

            }
        }
        if (splitFiles.size() > 0) {
            mergeFiles(resultDB, resultDBIndex, splitFiles);
            hasResult = true;
        }
    } else if (splitProcessCount == 1) {
        if (runSplit(qdbr, resultDB.c_str(), resultDBIndex.c_str(), fromSplit, totalSplits, sameQTDB)) {
            hasResult = true;
        }
    }

    if (sameQTDB == false) {
        qdbr->close();
        delete qdbr;
    }

    return hasResult;
}

bool Prefiltering::runSplit(DBReader<unsigned int>* qdbr, const std::string &resultDB, const std::string &resultDBIndex,
                            size_t split, size_t splitCount, bool sameQTDB) {

    Debug(Debug::INFO) << "Process prefiltering step " << (split + 1) << " of " << splitCount << "\n\n";

    size_t dbFrom = 0;
    size_t dbSize = tdbr->getSize();
    size_t queryFrom = 0;
    size_t querySize = qdbr->getSize();

    size_t maxResults = maxResListLen;
    if (splitCount > 1) {
        size_t fourTimesStdDeviation = 4*sqrt(static_cast<double>(maxResListLen) / static_cast<double>(splitCount));
        maxResults = (maxResListLen / splitCount) + std::max(static_cast<size_t >(1), fourTimesStdDeviation);
    }

    // create index table based on split parameter
    if (splitMode == Parameters::TARGET_DB_SPLIT) {
        Util::decomposeDomainByAminoAcid(tdbr->getAminoAcidDBSize(), tdbr->getSeqLens(), tdbr->getSize(),
                                         split, splitCount, &dbFrom, &dbSize);
        if (dbSize == 0) {
            return false;
        }

        if (indexTable != NULL) {
            delete indexTable;
            indexTable = NULL;
        }

        if (sequenceLookup != NULL) {
            delete sequenceLookup;
            sequenceLookup = NULL;
        }

        if(splitCount != (size_t) splits) {
            reopenTargetDb();
            if (sameQTDB == true) {
                qdbr = tdbr;
            }
        }

        getIndexTable(split, dbFrom, dbSize);
    } else if (splitMode == Parameters::QUERY_DB_SPLIT) {
        Util::decomposeDomainByAminoAcid(qdbr->getAminoAcidDBSize(), qdbr->getSeqLens(), qdbr->getSize(),
                                         split, splitCount, &queryFrom, &querySize);
        if (querySize == 0) {
            return false;
        }
    }

    double kmerMatchProb;
    if (diagonalScoring) {
        kmerMatchProb = 0.0f;
    } else {
        // run small query sample against the index table to calibrate p-match
        kmerMatchProb = setKmerThreshold(qdbr);
    }
    Debug(Debug::INFO) << "k-mer similarity threshold: " << kmerThr << "\n";
    Debug(Debug::INFO) << "k-mer match probability: " << kmerMatchProb << "\n\n";

    Timer timer;

    size_t kmersPerPos = 0;
    size_t dbMatches = 0;
    size_t doubleMatches = 0;
    size_t querySeqLenSum = 0;
    size_t resSize = 0;
    size_t realResSize = 0;
    size_t diagonalOverflow = 0;
    size_t totalQueryDBSize = querySize;

#ifdef OPENMP
    unsigned int totalThreads = threads;
#else
    unsigned int totalThreads = 1;
#endif

    unsigned int localThreads = totalThreads;
    if (querySize <= totalThreads) {
        localThreads = querySize;
    }

    DBWriter tmpDbw(resultDB.c_str(), resultDBIndex.c_str(), localThreads);
    tmpDbw.open();

    // init all thread-specific data structures
    char *notEmpty = new char[querySize];
    memset(notEmpty, 0, querySize * sizeof(char)); // init notEmpty

    std::list<int> **reslens = new std::list<int> *[localThreads];
    #pragma omp parallel num_threads(localThreads)
    {
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif
        reslens[thread_idx] = new std::list<int>();
    }

    Debug(Debug::INFO) << "Starting prefiltering scores calculation (step " << (split + 1) << " of " << splitCount << ")\n";
    Debug(Debug::INFO) << "Query db start  " << (queryFrom + 1) << " to " << queryFrom + querySize << "\n";
    Debug(Debug::INFO) << "Target db start  " << (dbFrom + 1) << " to " << dbFrom + dbSize << "\n";
    EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), subMat);

#pragma omp parallel num_threads(localThreads)
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
        Sequence seq(maxSeqLen, querySeqType, subMat, kmerSize, spacedKmer, aaBiasCorrection, true, spacedKmerPattern);

        QueryMatcher matcher(indexTable, sequenceLookup, subMat, evaluer, tdbr->getSeqLens() + dbFrom, kmerThr, kmerMatchProb,
                             kmerSize, dbSize, maxSeqLen, seq.getEffectiveKmerSize(),
                             maxResults, aaBiasCorrection, diagonalScoring, minDiagScoreThr, takeOnlyBestKmer);

        if (querySeqType == Sequence::HMM_PROFILE || querySeqType == Sequence::PROFILE_STATE_PROFILE) {
            matcher.setProfileMatrix(seq.profile_matrix);
        } else {
            matcher.setSubstitutionMatrix(_3merSubMatrix, _2merSubMatrix);
        }

#pragma omp for schedule(dynamic, 10) reduction (+: kmersPerPos, resSize, dbMatches, doubleMatches, querySeqLenSum, diagonalOverflow)
        for (size_t id = queryFrom; id < queryFrom + querySize; id++) {
            Debug::printProgress(id);
            // get query sequence
            char *seqData = qdbr->getData(id);
            unsigned int qKey = qdbr->getDbKey(id);
            seq.mapSequence(id, qKey, seqData);
            // only the corresponding split should include the id (hack for the hack)
            size_t targetSeqId = UINT_MAX;
            if (id >= dbFrom && id < (dbFrom + dbSize) && (sameQTDB || includeIdentical)) {
                targetSeqId = tdbr->getId(seq.getDbKey());
                if (targetSeqId != UINT_MAX) {
                    targetSeqId = targetSeqId - dbFrom;
                }
            }
            // calculate prefiltering results
            std::pair<hit_t *, size_t> prefResults = matcher.matchQuery(&seq, targetSeqId);
            size_t resultSize = prefResults.second;
            // write
            writePrefilterOutput(qdbr, &tmpDbw, thread_idx, id, prefResults, dbFrom, resListOffset, maxResults);

            // update statistics counters
            if (resultSize != 0) {
                notEmpty[id - queryFrom] = 1;
            }

            kmersPerPos += (size_t) matcher.getStatistics()->kmersPerPos;
            dbMatches += matcher.getStatistics()->dbMatches;
            doubleMatches += matcher.getStatistics()->doubleMatches;
            querySeqLenSum += seq.L;
            diagonalOverflow += matcher.getStatistics()->diagonalOverflow;
            resSize += resultSize;
            realResSize += std::min(resultSize, maxResults);
            reslens[thread_idx]->emplace_back(resultSize);
        } // step end
    }

    if (Debug::debugLevel >= Debug::INFO) {
        statistics_t stats(kmersPerPos / totalQueryDBSize,
                           dbMatches / totalQueryDBSize,
                           doubleMatches / totalQueryDBSize,
                           querySeqLenSum, diagonalOverflow,
                           resSize / totalQueryDBSize);

        size_t empty = 0;
        for (size_t id = 0; id < querySize; id++) {
            if (notEmpty[id] == 0) {
                empty++;
            }
        }

        printStatistics(stats, reslens, localThreads, empty, maxResults);
    }
    Debug(Debug::INFO) << "\nTime for prefiltering scores calculation: " << timer.lap() << "\n";
    tmpDbw.close(); // sorts the index

    // sort by ids
    // needed to speed up merge later one
    // sorts this datafile according to the index file
    if (splitCount > 1 && splitMode == Parameters::TARGET_DB_SPLIT) {
        DBReader<unsigned int> resultReader(tmpDbw.getDataFileName(), tmpDbw.getIndexFileName());
        resultReader.open(DBReader<unsigned int>::NOSORT);
        DBWriter resultWriter((resultDB + "_tmp").c_str(), (resultDBIndex + "_tmp").c_str(), localThreads);
        resultWriter.open();
        resultWriter.sortDatafileByIdOrder(resultReader);
        resultWriter.close();
        resultReader.close();
        remove(resultDB.c_str());
        remove(resultDBIndex.c_str());
        std::rename((resultDB + "_tmp").c_str(), resultDB.c_str());
        std::rename((resultDBIndex + "_tmp").c_str(), resultDBIndex.c_str());
    }

    for (unsigned int i = 0; i < localThreads; i++) {
        reslens[i]->clear();
        delete reslens[i];
    }
    delete[] reslens;
    delete[] notEmpty;

    return true;
}

// write prefiltering to ffindex database
void Prefiltering::writePrefilterOutput(DBReader<unsigned int> *qdbr, DBWriter *dbWriter, unsigned int thread_idx, size_t id,
                                        const std::pair<hit_t *, size_t> &prefResults, size_t seqIdOffset,
                                        size_t resultOffsetPos, size_t maxResults) {
    // write prefiltering results to a string
    size_t l = 0;
    hit_t *resultVector = prefResults.first + resultOffsetPos;
    const size_t resultSize = (prefResults.second < resultOffsetPos) ? 0 : prefResults.second - resultOffsetPos;
    std::string prefResultsOutString;
    prefResultsOutString.reserve(BUFFER_SIZE);
    char buffer[100];
    for (size_t i = 0; i < resultSize; i++) {
        hit_t *res = resultVector + i;
        size_t targetSeqId = res->seqId + seqIdOffset;
        if (targetSeqId >= tdbr->getSize()) {
            Debug(Debug::INFO) << "Wrong prefiltering result: Query: " << qdbr->getDbKey(id) << " -> " << targetSeqId
                               << "\t" << res->prefScore << "\n";
        }
        if(covThr > 0.0 && (covMode == Parameters::COV_MODE_BIDIRECTIONAL || covMode == Parameters::COV_MODE_QUERY)){
            float queryLength = static_cast<float>(qdbr->getSeqLens(id));
            float targetLength = static_cast<float>(tdbr->getSeqLens(targetSeqId));
            if(Util::canBeCovered(covThr, covMode, queryLength, targetLength)==false){
                continue;
            }
        }


        res->seqId = tdbr->getDbKey(targetSeqId);
        int len = QueryMatcher::prefilterHitToBuffer(buffer, *res);
        // TODO: error handling for len
        prefResultsOutString.append(buffer, len);
        l++;
        // maximum allowed result list length is reached
        if (l >= maxResults)
            break;
    }
    // write prefiltering results string to ffindex database
    const size_t prefResultsLength = prefResultsOutString.length();
    char *prefResultsOutData = (char *) prefResultsOutString.c_str();
    dbWriter->writeData(prefResultsOutData, prefResultsLength, qdbr->getDbKey(id), thread_idx);
}

void Prefiltering::printStatistics(const statistics_t &stats, std::list<int> **reslens,
                                   unsigned int resLensSize, size_t empty, size_t maxResults) {
    // sort and merge the result list lengths (for median calculation)
    reslens[0]->sort();
    for (unsigned int i = 1; i < resLensSize; i++) {
        reslens[i]->sort();
        reslens[0]->merge(*reslens[i]);
    }
    Debug(Debug::INFO) << "\n" << stats.kmersPerPos << " k-mers per position.\n";
    Debug(Debug::INFO) << stats.dbMatches << " DB matches per sequence.\n";
    Debug(Debug::INFO) << stats.diagonalOverflow << " Overflows.\n";
    Debug(Debug::INFO) << stats.resultsPassedPrefPerSeq << " sequences passed prefiltering per query sequence";
    if (stats.resultsPassedPrefPerSeq > maxResults)
        Debug(Debug::INFO) << " (ATTENTION: max. " << maxResults
                           << " best scoring sequences were written to the output prefiltering database).\n";
    else
        Debug(Debug::INFO) << ".\n";
    size_t mid = reslens[0]->size() / 2;
    std::list<int>::iterator it = reslens[0]->begin();
    std::advance(it, mid);
    Debug(Debug::INFO) << "Median result list size: " << *it << "\n";
    Debug(Debug::INFO) << empty << " sequences with 0 size result lists.\n";
}

BaseMatrix *Prefiltering::getSubstitutionMatrix(const std::string &scoringMatrixFile, size_t alphabetSize, float bitFactor, bool profileState) {
    Debug(Debug::INFO) << "Substitution matrices...\n";
    BaseMatrix *subMat;
    if (alphabetSize < 21) {
        SubstitutionMatrix sMat(scoringMatrixFile.c_str(), bitFactor, -0.2f);
        subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, sMat.aa2int, sMat.int2aa, sMat.alphabetSize, alphabetSize, bitFactor);
    }else if(profileState == true){
        SubstitutionMatrix sMat(scoringMatrixFile.c_str(), bitFactor, -0.2f);
        subMat = new SubstitutionMatrixProfileStates(sMat.matrixName, sMat.probMatrix, sMat.pBack,
                                                     sMat.subMatrixPseudoCounts, bitFactor, 0.0, 8);
    } else {
        subMat = new SubstitutionMatrix(scoringMatrixFile.c_str(), bitFactor, -0.2f);
    }
    return subMat;
}

double Prefiltering::setKmerThreshold(DBReader<unsigned int> *qdbr) {
    // generate a small random sequence set for testing
    size_t querySetSize = std::min(qdbr->getSize(), (size_t) 1000);
    unsigned int *querySeqs = new unsigned int[querySetSize];
    srand(1);
    for (size_t i = 0; i < querySetSize; i++) {
        querySeqs[i] = rand() % qdbr->getSize();
    }

    double kmersPerPos = 0.0;
    size_t doubleMatches = 0;
    size_t querySeqLenSum = 0;

    unsigned int effectiveKmerSize = 0;

    #pragma omp parallel shared(effectiveKmerSize)
    {
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif
        Sequence seq(maxSeqLen, querySeqType, subMat, kmerSize, spacedKmer, aaBiasCorrection, true, spacedKmerPattern);

        if (thread_idx == 0) {
            effectiveKmerSize = seq.getEffectiveKmerSize();
        }

        EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), subMat);
        QueryMatcher matcher(indexTable, sequenceLookup, subMat, evaluer, tdbr->getSeqLens(), kmerThr, 1.0,
                             kmerSize, indexTable->getSize(), maxSeqLen, seq.getEffectiveKmerSize(),
                             150000, aaBiasCorrection, false, minDiagScoreThr, takeOnlyBestKmer);
        if(querySeqType == Sequence::HMM_PROFILE || querySeqType == Sequence::PROFILE_STATE_PROFILE ){
            matcher.setProfileMatrix(seq.profile_matrix);
        } else {
            matcher.setSubstitutionMatrix(_3merSubMatrix, _2merSubMatrix);
        }

        #pragma omp for schedule(dynamic, 10) reduction (+: doubleMatches, kmersPerPos, querySeqLenSum)
        for (size_t i = 0; i < querySetSize; i++) {
            size_t id = querySeqs[i];

            char *seqData = qdbr->getData(id);
            seq.mapSequence(id, 0, seqData);
            seq.reverse();

            matcher.matchQuery(&seq, UINT_MAX);
            kmersPerPos += matcher.getStatistics()->kmersPerPos;
            querySeqLenSum += matcher.getStatistics()->querySeqLen;
            doubleMatches += matcher.getStatistics()->doubleMatches;
        }
    }

    size_t targetDbSize = indexTable->getSize();
    size_t targetSeqLenSum = 0;

    unsigned int* tSeqLens = tdbr->getSeqLens();
    for (size_t i = 0; i < targetDbSize; i++) {
        targetSeqLenSum += (tSeqLens[i] - effectiveKmerSize);
    }

    // compute match prob for local match
    double kmerMatchProb = ((double) doubleMatches) / ((double) (querySeqLenSum * targetSeqLenSum));
    kmerMatchProb /= 256;

    kmerMatchProb = std::max(kmerMatchProb, std::numeric_limits<double>::min());
    Debug(Debug::INFO) << "\tk-mers per position = " << (kmersPerPos / (double) querySetSize)
                       << ", k-mer match probability: " << kmerMatchProb << "\n";
    delete[] querySeqs;
    return kmerMatchProb;
}

void Prefiltering::mergeFiles(const std::string &outDB, const std::string &outDBIndex,
                              const std::vector<std::pair<std::string, std::string>> &splitFiles) {
    if (splitMode == Parameters::TARGET_DB_SPLIT) {
        mergeOutput(outDB, outDBIndex, splitFiles);
    } else if (splitMode == Parameters::QUERY_DB_SPLIT) {
        DBWriter::mergeResults(outDB, outDBIndex, splitFiles);
    }
}

int Prefiltering::getKmerThreshold(const float sensitivity, const int querySeqType,
                                   const int kmerScore, const int kmerSize) {
    double kmerThrBest = kmerScore;
    if (kmerScore == INT_MAX) {
        if (kmerSize == 5) {
            float base = 123.75;
            if (querySeqType == Sequence::HMM_PROFILE) {
                base += 17.0;
            }
            kmerThrBest = base - (sensitivity * 8.75);
        } else if (kmerSize == 6) {
            float base = 138.75;
            if (querySeqType == Sequence::HMM_PROFILE) {
                base += 17.0;
            }
            kmerThrBest = base - (sensitivity * 8.75);
        } else if (kmerSize == 7) {
            float base = 154.75;
            if (querySeqType == Sequence::HMM_PROFILE) {
                base += 17.0;
            }
            kmerThrBest = base - (sensitivity * 9.75);
        } else {
            Debug(Debug::ERROR) << "The k-mer size " << kmerSize << " is not valid.\n";
            EXIT(EXIT_FAILURE);
        }
    }
    return static_cast<int>(kmerThrBest);
}

size_t Prefiltering::estimateMemoryConsumption(int split, size_t dbSize, size_t resSize,
                                               size_t maxHitsPerQuery,
                                               int alphabetSize, int kmerSize, unsigned int querySeqType,
                                               int threads) {
    // for each residue in the database we need 7 byte
    size_t dbSizeSplit = (dbSize) / split;
    size_t residueSize = (resSize / split * 7);
    // 21^7 * pointer size is needed for the index
    size_t indexTableSize = static_cast<size_t>(pow(alphabetSize, kmerSize)) * sizeof(size_t *);
    // memory needed for the threads
    // This memory is an approx. for Countint32Array and QueryTemplateLocalFast
    size_t threadSize = threads * (
            (dbSizeSplit * 2 * sizeof(IndexEntryLocal)) // databaseHits in QueryMatcher
            + (dbSizeSplit * sizeof(CounterResult)) // databaseHits in QueryMatcher
            + (maxHitsPerQuery * sizeof(hit_t))
            + (dbSizeSplit * 2 * sizeof(CounterResult) * 2) // BINS * binSize, (binSize = dbSize * 2 / BINS)
            // 2 is a security factor the size can increase during run
    );

    // extended matrix
    size_t extendedMatrix = 0;
    if(querySeqType == Sequence::AMINO_ACIDS){
        extendedMatrix = sizeof(std::pair<short, unsigned int>) * static_cast<size_t>(pow(pow(alphabetSize, 3), 2));
        extendedMatrix += sizeof(std::pair<short, unsigned int>) * pow(pow(alphabetSize, 2), 2);
    }
    // some memory needed to keep the index, ....
    size_t background = dbSize * 22;
    return residueSize + indexTableSize + threadSize + background + extendedMatrix;
}

size_t Prefiltering::estimateHDDMemoryConsumption(size_t dbSize, size_t maxResListLen) {
    // 21 bytes is roughly the size of an entry
    // 2x because the merge doubles the hdd demand
    return 2 * (21 * dbSize * maxResListLen);
}

std::pair<int, int> Prefiltering::optimizeSplit(size_t totalMemoryInByte, DBReader<unsigned int> *tdbr,
                                                int alphabetSize, int externalKmerSize, unsigned int querySeqType, unsigned int threads) {
    for (int optSplit = 1; optSplit < 100; optSplit++) {
        for (int optKmerSize = 6; optKmerSize <= 7; optKmerSize++) {
            if (optKmerSize == externalKmerSize || externalKmerSize == 0) { // 0: set k-mer based on aa size in database
                size_t aaUpperBoundForKmerSize = IndexTable::getUpperBoundAACountForKmerSize(optKmerSize);
                if ((tdbr->getAminoAcidDBSize() / optSplit) < aaUpperBoundForKmerSize) {
                    size_t neededSize = estimateMemoryConsumption(optSplit, tdbr->getSize(), tdbr->getAminoAcidDBSize(),
                                                                  0, alphabetSize, optKmerSize, querySeqType, threads);
                    if (neededSize < 0.9 * totalMemoryInByte) {
                        return std::make_pair(optKmerSize, optSplit);
                    }
                }
            }
        }
    }

    return std::make_pair(-1, -1);
}

