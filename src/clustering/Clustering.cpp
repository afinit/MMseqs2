#include "Clustering.h"

Clustering::Clustering(std::string seqDB, std::string seqDBIndex,
        std::string alnDB, std::string alnDBIndex,
        std::string outDB, std::string outDBIndex,
        float seqIdThr){

    std::cout << "Init...\n";
    std::cout << "Opening sequence database...\n";
    seqDbr = new DBReader(seqDB.c_str(), seqDBIndex.c_str());
    seqDbr->open(DBReader::SORT);

    std::cout << "Opening alignment database...\n";
    alnDbr = new DBReader(alnDB.c_str(), alnDBIndex.c_str());
    alnDbr->open(DBReader::NOSORT);

    dbw = new DBWriter(outDB.c_str(), outDBIndex.c_str());
    dbw->open();

    this->seqIdThr = seqIdThr;
    std::cout << "done.\n";
}

void Clustering::run(int mode){

    struct timeval start, end;
    gettimeofday(&start, NULL);

    std::list<set *> ret;
    Clustering::set_data set_data;
    if (mode == SET_COVER){
        std::cout << "Clustering mode: SET COVER\n";
        std::cout << "Reading the data...\n";
        set_data = read_in_set_data();

        std::cout << "Init set cover...\n";
        SetCover setcover(set_data.set_count,
                set_data.uniqu_element_count,
                set_data.max_weight,
                set_data.all_element_count,
                set_data.element_size_lookup
                );

        for(size_t i = 0; i < set_data.set_count; i++){
            setcover.add_set(i+1, set_data.set_sizes[i]
                    ,(const unsigned int*)set_data.sets[i],
                    (const unsigned short*)set_data.weights[i],
                    set_data.set_sizes[i]);
        }
        std::cout.flush();

        std::cout << "Clustering...\n";
        ret = setcover.execute_set_cover();
        std::cout << "done.\n";

        for(size_t i = 0; i < set_data.set_count; i++){
            delete[] set_data.sets[i];
            delete[] set_data.weights[i];

        }
        delete[] set_data.sets;
        delete[] set_data.weights;
        delete[] set_data.set_sizes;
        delete[] set_data.element_size_lookup;


    }
    else if (mode == GREEDY){

        std::cout << "Clustering mode: GREEDY\n";
        std::cout << "Reading the data...\n";
        set_data = read_in_set_data();

        std::cout << "Init simple clustering...\n";
        SimpleClustering simpleClustering(set_data.set_count,
                set_data.uniqu_element_count,
                set_data.all_element_count,
                set_data.element_size_lookup);

        for(size_t i = 0; i < set_data.set_count; i++){
            simpleClustering.add_set((const unsigned int*)set_data.sets[i],
                    set_data.set_sizes[i]);
            delete[] set_data.sets[i];
        }
        delete[] set_data.sets;

        std::cout.flush();

        std::cout << "Clustering...\n";
        ret = simpleClustering.execute();
        std::cout << "done.\n";

    }
    else{
        std::cerr << "ERROR: Wrong clustering mode!\n";
        exit(EXIT_FAILURE);
    }
    std::cout << "Validating results...\n";
    if(validate_result(&ret,set_data.uniqu_element_count))
        std::cout << " VALID\n";
    else
        std::cout << " NOT VALID\n";

    std::cout.flush();

    int dbSize = alnDbr->getSize();
    int seqDbSize = seqDbr->getSize();
    int cluNum = ret.size();

    std::cout << "Writing results...\n";
    writeData(ret);
    seqDbr->close();
    alnDbr->close();
    dbw->close();
    std::cout << "...done.\n";

    gettimeofday(&end, NULL);
    int sec = end.tv_sec - start.tv_sec;
    std::cout << "\nTime for clustering: " << (sec / 60) << " m " << (sec % 60) << "s\n\n";

    std::cout << "\nSize of the sequence database: " << seqDbSize << "\n";
    std::cout << "Size of the alignment database: " << dbSize << "\n";
    std::cout << "Number of clusters: " << cluNum << "\n";

}

void Clustering::writeData(std::list<set *> ret){

    size_t BUFFER_SIZE = 1000000;
    char* outBuffer = new char[BUFFER_SIZE];
    std::list<set *>::const_iterator iterator;
    for (iterator = ret.begin(); iterator != ret.end(); ++iterator) {
        std::stringstream res;
        set::element * element =(*iterator)->elements;
        // first entry is the representative sequence
        char* dbKey = seqDbr->getDbKey(element->element_id);
        do{ 
            char* nextDbKey = seqDbr->getDbKey(element->element_id);
            res << nextDbKey << "\n";
        }while((element=element->next)!=NULL);

        std::string cluResultsOutString = res.str();
        const char* cluResultsOutData = cluResultsOutString.c_str();
        if (BUFFER_SIZE < strlen(cluResultsOutData)){
            std::cerr << "Tried to process the clustering list for the query " << dbKey << " , length of the list = " << ret.size() << "\n";
            std::cerr << "Output buffer size < clustering result size! (" << BUFFER_SIZE << " < " << cluResultsOutString.length() << ")\nIncrease buffer size or reconsider your parameters -> output buffer is already huge ;-)\n";
            continue;
        }
        memcpy(outBuffer, cluResultsOutData, cluResultsOutString.length()*sizeof(char));
        dbw->write(outBuffer, cluResultsOutString.length(), dbKey);
    }
    delete[] outBuffer;
}


bool Clustering::validate_result(std::list<set *> * ret,unsigned int uniqu_element_count){
    std::list<set *>::const_iterator iterator;
    unsigned int result_element_count=0;
    int* control = new int [uniqu_element_count+1];
    memset(control, 0, sizeof(int)*(uniqu_element_count+1));
    for (iterator = ret->begin(); iterator != ret->end(); ++iterator) {
        set::element * element =(*iterator)->elements;
        do{ 
            control[element->element_id]++;
            result_element_count++;
        }while((element=element->next)!=NULL);
    }
    int notin = 0;
    int toomuch = 0;
    for (int i = 0; i < uniqu_element_count; i++){
        if (control[i] == 0){
            std::cout << "id " << i << " (key " << seqDbr->getDbKey(i) << ") is not in the clustering!\n";
            notin++;
        }
        else if (control[i] > 1){
            std::cout << "id " << i << " (key " << seqDbr->getDbKey(i) << ") is " << control[i] << " times in the clustering!\n";
            toomuch = toomuch + control[i];
        }
    }
    std::cout << "not in: " << notin << "\n";
    std::cout << "too much: " << toomuch << "\n";
    delete[] control;
    if (uniqu_element_count==result_element_count)
        return true;
    else{
        std::cerr << "uniqu_element_count: " << uniqu_element_count << ", result_element_count: " << result_element_count << "\n";
        return false;
    }
}


Clustering::set_data Clustering::read_in_set_data(){

    Clustering::set_data ret_struct;

    // n = overall sequence count
    int n = seqDbr->getSize();
    // m = number of sets
    int m = alnDbr->getSize();

    ret_struct.uniqu_element_count = n;
    ret_struct.set_count = m;

    unsigned int * element_buffer=(unsigned int *)malloc(n * sizeof(unsigned int));
    unsigned int * element_size=new unsigned int[n+2];
    memset(element_size, 0, sizeof(unsigned int)*(n+2));
    ret_struct.element_size_lookup = element_size;
    unsigned int * set_size=new unsigned int[m];

    ret_struct.set_sizes = set_size;
    unsigned int ** sets   = new unsigned int*[m];
    ret_struct.sets = sets;
    unsigned short ** weights = new unsigned short*[m];
    ret_struct.weights = weights;
    ret_struct.max_weight = 0;
    ret_struct.all_element_count=0;

    char* buf = new char[1000000];

    int empty = 0;

    // the reference id of the elements is always their id in the sequence database
    for(int i = 0; i<m; i++){
        if (i % 1000000 == 0 && i > 0){
            std::cout << "\t" << (i/1000000) << " Mio. sequences processed\n";
            fflush(stdout);
        }
        else if (i % 10000 == 0 && i > 0) {
            std::cout << ".";
            fflush(stdout);
        }

        char* data = alnDbr->getData(i);
        strcpy(buf, data);
        unsigned int element_counter=0;

        // add the element itself to its own cluster
/*        int rep_element_id = seqDbr->getId(alnDbr->getDbKey(i));
        element_buffer[element_counter++]=rep_element_id;
        element_size[rep_element_id]++;
        ret_struct.all_element_count++;
*/
        char* prevKey = 0;
        char* dbKey = strtok(buf, "\t");

        int cnt = 0;
        while (dbKey != 0)
        {
            if (prevKey != 0 && strcmp(prevKey, dbKey) == 0){
                prevKey = dbKey;
                dbKey = strtok(NULL, "\n");
                dbKey = strtok(NULL, "\t");
                continue;
            }
            prevKey = dbKey;

            size_t curr_element = seqDbr->getId(dbKey);
            if (curr_element == UINT_MAX){
                std::cerr << "ERROR: Element " << dbKey << " contained in some alignment list, but not contained in the sequence database!\n";
                exit(1);
            }
            int score = atoi(strtok(NULL, "\t")); // score
            float qcov = atof(strtok(NULL, "\t")); // query sequence coverage
            float dbcov = atof(strtok(NULL, "\t")); // db sequence coverage
            float seqId = atof(strtok(NULL, "\t")); // sequence identity
            double eval = atof(strtok(NULL, "\n")); // e-value
            // add an edge if it meets the thresholds
            if (seqId >= seqIdThr){
                element_buffer[element_counter++]=curr_element;
                element_size[curr_element]++;
                ret_struct.all_element_count++;
            }
            // next db key
            dbKey = strtok(NULL, "\t");
            cnt++;
        }

        if (cnt == 0)
            empty++;

        if(ret_struct.max_weight < element_counter){
            ret_struct.max_weight = element_counter;
        }

        unsigned int * elements = new unsigned int[element_counter];
        memcpy(elements,element_buffer,sizeof(int)*element_counter);
        unsigned short * weight = new unsigned short[element_counter];
        std::fill_n(weight, element_counter, 1);

        weights[i] = weight;
        sets[i] = elements;
        set_size[i]=element_counter;

    }

    if (empty > 0)
        std::cout << empty << " input sets were empty!\n";
    free(element_buffer);
    delete[] buf;

    return ret_struct;
}

