#ifndef CLUSTERING_H
#define CLUSTERING_H

#include <list>
#include <string>
#include <unordered_map>

#include "DBReader.h"
#include "DBWriter.h"
#include "SetElement.h"

class Clustering {
public:
    Clustering(const std::string &seqDB, const std::string &seqDBIndex,
               const std::string &alnResultsDB, const std::string &alnResultsDBIndex,
               const std::string &outDB, const std::string &outDBIndex,
               unsigned int maxIteration, int similarityScoreType, int threads);

    void run(int mode);


    ~Clustering();

private:

    void writeData(DBWriter *dbw, const std::unordered_map<unsigned int, std::vector<unsigned int>> &ret);

    DBReader<unsigned int> *seqDbr;
    DBReader<unsigned int> *alnDbr;

    //values for affinity clustering
    unsigned int maxIteration;
    int similarityScoreType;

    int threads;
    std::string outDB;
    std::string outDBIndex;
};

#endif
