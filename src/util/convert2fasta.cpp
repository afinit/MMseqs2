/*
 * convert2fasta
 * written by Milot Mirdita <milot@mirdita.de>
 */

#include <cstring>
#include <cstdio>

#include "Parameters.h"
#include "DBReader.h"
#include "Debug.h"
#include "Util.h"
#include "FileUtil.h"

const char header_start[] = {'>'};
const char newline[] = {'\n'};

int convert2fasta(int argc, const char **argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 2);

    DBReader<unsigned int> db(par.db1.c_str(), par.db1Index.c_str());
    db.open(DBReader<unsigned int>::NOSORT);

    DBReader<unsigned int> db_header(par.hdr1.c_str(), par.hdr1Index.c_str());
    db_header.open(DBReader<unsigned int>::NOSORT);

    FILE *fastaFP =  FileUtil::openFileOrDie(par.db2.c_str(), "w", false);

    DBReader<unsigned int>* from = &db;
    if(par.useHeaderFile) {
        from = &db_header;
    }

    Debug(Debug::INFO) << "Start writing file to " << par.db2 << "\n";
    for(size_t i = 0; i < from->getSize(); i++){
        unsigned int key = from->getDbKey(i);

        const char* header_data = db_header.getDataByDBKey(key);

        fwrite(header_start, sizeof(char), 1, fastaFP);
        fwrite(header_data, sizeof(char), strlen(header_data) - 1, fastaFP);
        fwrite(newline, sizeof(char), 1, fastaFP);

        const char* body_data = db.getDataByDBKey(key);
        fwrite(body_data, sizeof(char), strlen(body_data) - 1, fastaFP);
        fwrite(newline, sizeof(char), 1, fastaFP);
    }

    fclose(fastaFP);
    db_header.close();
    db.close();

    return EXIT_SUCCESS;
}
