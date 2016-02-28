//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// OtherActions.cpp -- more CNTK actions
//

#define _CRT_NONSTDC_NO_DEPRECATE // make VS accept POSIX functions without _

#include "stdafx.h"
#include "Basics.h"
#include "Actions.h"
#include "ComputationNetwork.h"
#include "ComputationNode.h"
#include "Config.h"
#include "ScriptableObjects.h"
#include "BrainScriptEvaluator.h"

#include <string>
#include <chrono>
#include <algorithm>
#include <vector>
#include <iostream>
#include <queue>
#include <set>
#include <memory>
#include <map>

#ifndef let
#define let const auto
#endif

using namespace std;
using namespace Microsoft::MSR;
using namespace Microsoft::MSR::CNTK;

// ===========================================================================
// DoCreateLabelMap() - implements CNTK "createLabelMap" command
// ===========================================================================

template <typename ElemType>
void DoCreateLabelMap(const ConfigParameters& config)
{
    // this gets the section name we are interested in
    std::string section = config(L"section");
    // get that section (probably a peer config section, which works thanks to heirarchal symbol resolution)
    ConfigParameters configSection(config(section));
    ConfigParameters readerConfig(configSection("reader"));
    readerConfig.Insert("allowMapCreation", "true");
    size_t minibatchSize = config(L"minibatchSize", "2048");
    int traceLevel = config(L"traceLevel", "0");
    std::vector<std::wstring> featureNames;
    std::vector<std::wstring> labelNames;
    GetFileConfigNames(readerConfig, featureNames, labelNames);

    // setup minibatch matrices
    Matrix<ElemType> featuresMatrix(CPUDEVICE);
    Matrix<ElemType> labelsMatrix(CPUDEVICE);
    StreamMinibatchInputs<ElemType> matrices;
    matrices[featureNames[0]] = &featuresMatrix;
    if (labelNames.size() == 0)
        RuntimeError("CreateLabelMap: no labels found to process");

    // now create the reader and loop through the entire dataset to get all the labels
    auto start = std::chrono::system_clock::now();
    for (const std::wstring& labelsName : labelNames)
    {
        // take the last label file defined (the other one might be input)
        matrices[labelsName] = &labelsMatrix;

        // get the label mapping file name
        ConfigParameters labelConfig(readerConfig(labelsName));
        std::string labelMappingFile;
        if (labelConfig.ExistsCurrent(L"labelMappingFile"))
        {
            labelMappingFile = labelConfig(L"labelMappingFile");
        }
        else if (readerConfig.ExistsCurrent(L"labelMappingFile"))
        {
            labelMappingFile = labelConfig(L"labelMappingFile");
        }
        else
        {
            RuntimeError("CreateLabelMap: No labelMappingFile defined");
        }

        if (fexists(labelMappingFile))
        {
            fprintf(stderr, "CreateLabelMap: the label mapping file '%s' already exists, no work to do.\n", labelMappingFile.c_str());
            return;
        }
        fprintf(stderr, "CreateLabelMap: Creating the mapping file '%s' \n", labelMappingFile.c_str());

        DataReader<ElemType> dataReader(readerConfig);

        dataReader.StartMinibatchLoop(minibatchSize, 0, requestDataSize);
        int count = 0;
        while (dataReader.GetMinibatch(matrices))
        {
            Matrix<ElemType>& features = *matrices[featureNames[0]];
            count += features.GetNumCols();
            if (traceLevel > 1)
                fprintf(stderr, "."); // progress meter
        }
        dataReader.StartMinibatchLoop(minibatchSize, 1, requestDataSize);

        // print the results
        if (traceLevel > 0)
            fprintf(stderr, "\nread %d labels and produced %s\n", count, labelMappingFile.c_str());
    }
    auto end = std::chrono::system_clock::now();
    auto elapsed = end - start;
    if (traceLevel > 1)
        fprintf(stderr, "%f seconds elapsed\n", (float) (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) / 1000);
}

template void DoCreateLabelMap<float>(const ConfigParameters& config);
template void DoCreateLabelMap<double>(const ConfigParameters& config);

// ===========================================================================
// DoParameterSVD() - implements CNTK "SVD" command
// ===========================================================================

//////////////////////////////////////////////////////////////////////////
//  for action SVD
//      An action "SVD" performs the following process to transform an existing model:
//          1.  For a Learnable Parameter A whose name matches with the user specified regex,
//              A is approximated by two matrice multiplication B*C ;
//          2.  In order to keep the low-rank structure in training,
//              the original A node will be replaced by A' whose opertions is Times
//              with its left children being B and right chilren being
//
//      To use this command,
//          user need to specify:
//                  1)  modelPath           -- path to the existing model
//                  2)  outputmodelPath     -- where to write the transformed model
//                  3)  KeepRatio           -- how many percentage of energy we want to keep
//                  4)  AlignedSize         -- the resultant number of signular values is aligned to e.g., 32 or 64
//                  5)  ParameterName       -- name (regex) of the parameter node we want to perform a SVD decomposition
//
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//  helper function for DoParameterSVD
//////////////////////////////////////////////////////////////////////////
bool ParseSVDConfigFile(wstring fn, map<wstring, float>& config)
{
    msra::files::textreader reader(fn);
    for (; reader;)
    {
        wstring line = reader.wgetline();
        vector<wstring> tokens = msra::strfun::split(line, L"\t ");
        if (tokens.size() != 2)
            return false;
        config[tokens[0]] = (float) msra::strfun::todouble(tokens[1]);
    }
    return true;
}
// a brief on the SVD config file usage
void SVDConfigFileUsage()
{
    fprintf(stderr, "usage of SVDConfigFile\n");
    fprintf(stderr, "A SVDConfigFile is referred in main config by \"SVDConfig\"\n");
    fprintf(stderr, "Each line in this file specifies a group of Learnable Parameter nodes using regex and the KeepRatio associated with that group\n");
    fprintf(stderr, "An example: \n");
    fprintf(stderr, "W0         1.0\n");
    fprintf(stderr, "W[1-5]     0.4\n");
}
template <typename ElemType>
void DoParameterSVD(const ConfigParameters& config)
{
    DEVICEID_TYPE deviceID = -1; // use CPU for SVD
    wstring modelPath = config(L"modelPath");
    wstring outputmodelPath = config(L"outputmodelPath");
    map<wstring, float> svdconfig;

    float keepratio = config(L"KeepRatio", "0.4");
    size_t AlignedSize = config(L"AlignedSize", "8");
    wstring svdnodeRegex = config(L"NodeNameRegex", L"");
    if (!svdnodeRegex.empty())
    {
        svdconfig[svdnodeRegex] = keepratio;
    }
    else
    {
        // alternatively, user can also use a config to specify KeepRatios for different groups of nodes
        wstring svdnodeConfigFile = config(L"SVDConfig", L"");
        if (!ParseSVDConfigFile(svdnodeConfigFile, svdconfig))
        {
            SVDConfigFileUsage();
            return;
        }
    }

    if (modelPath.empty())
    {
        fprintf(stderr, "ERROR: in DoParameterSVD, modelPath is empty!\n");
        return;
    }

    ComputationNetwork net(deviceID);
    net.Load<ElemType>(modelPath);

    net.PerformSVDecomposition<ElemType>(svdconfig, AlignedSize);
    if (!outputmodelPath.empty())
        net.Save(outputmodelPath);
}

template void DoParameterSVD<float>(const ConfigParameters& config);
template void DoParameterSVD<double>(const ConfigParameters& config);

// ===========================================================================
// DoWriteWordAndClassInfo() - implements CNTK "writeWordAndClass" command
// ===========================================================================

template <typename T>
struct compare_second
{
    bool operator()(const T& lhs, const T& rhs) const
    {
        return lhs.second < rhs.second;
    }
};

// This converts the training text file into a special format that encodes class information.
//
// The outputs are the vocabulary, word2class and class2idx file with the information below:
//     vocabulary format is as follows
//       0      42068  </s>    0
//       1      50770  the 0
//       2      45020  <unk>   1
//       the first column is word index
//       the last column is class index of the word
//       the second column and the third column are for information purpose and
//       are not really used in generating outputs for later process in the neural networks training
//
//    wrd2cls in dense matrix in[vocab_size X 1].it maps a word to its class id.
//    cls2idx in dense matrix in[nbr_cls X 1].it maps a class to its first word index.
//
// This format is for use with class-based entropy. The following assumptions are made:
// A1 : words are sorted so that words that are in the same class are together
//    i.e., wrds2cls[0] <= wrd2cls[1] <= ... <= wrd2cls[vocab_size - 1]
// A2 : class ids are sorted so that cls2idx[0] < cls2idx[1] < cls2idx[2] < ... < cls2idx[nbr_cls - 1]
template <typename ElemType>
void DoWriteWordAndClassInfo(const ConfigParameters& config)
{
    size_t vocabSize = config(L"vocabSize");
    int nbrCls = config(L"nbrClass", "0"); // TODO: why int and not size_t?
    int cutoff = config(L"cutoff", "1");

    string inputFile = config(L"inputFile"); // training text file without <unk>
    string outputVocabFile = config(L"outputVocabFile");
    string outputWord2Cls  = nbrCls > 0 ? config(L"outputWord2Cls")  : string();
    string outputCls2Index = nbrCls > 0 ? config(L"outputCls2Index") : string();

    string unkWord       = config(L"unk", "<unk>");
    string beginSequence = config(L"beginSequence", "");
    string endSequence   = config(L"endSequence",   "");
    // legacy: Old version hard-coded "</s>" for ^^ both of these.
    //         For a while, do not fall back to defaults but rather have users fix their scripts.
    if (beginSequence.empty() || endSequence.empty())
        InvalidArgument("Please specify parameters 'beginSequence' and 'endSequence'.");

    std::cerr     << "Vocabulary file:    " << outputVocabFile << std::endl;
    if (nbrCls > 0)
    {
        std::cerr << "Word-to-class map:  " << outputWord2Cls  << std::endl;
        std::cerr << "Class-to-index map: " << outputCls2Index << std::endl;
    }
    std::cerr << std::endl;
    
    // check whether we are already up-to-date
    bool makeMode = config(L"makeMode", true);
    if (makeMode)
    {
        bool done = msra::files::fuptodate(s2ws(outputVocabFile), s2ws(inputFile), /*inputRequired=*/false);
        if (nbrCls > 0)
        {
            done &= msra::files::fuptodate(s2ws(outputWord2Cls),  s2ws(inputFile), /*inputRequired=*/false);
            done &= msra::files::fuptodate(s2ws(outputCls2Index), s2ws(inputFile), /*inputRequired=*/false);
        }
        if (done)
        {
            std::cerr << "All output files up to date.\n";
            return;
        }
    }

    Matrix<ElemType> wrd2cls(CPUDEVICE);
    Matrix<ElemType> cls2idx(CPUDEVICE);

    ifstream fp(inputFile.c_str()); // TODO: use class File, as to support pipes
    if (!fp)
        RuntimeError("Failed to open input file: %s", inputFile.c_str());
    cerr << "Reading input file inputFile: " << inputFile << std::endl;

    if (nbrCls > 0)
        cls2idx.Resize(nbrCls, 1);

#if 1
    std::unordered_map<string, double> v_count;
#else
    // TODO: For unknown reasons, this gives a very different result (PPL of 500 instead of 190). Should be tracked down.
    std::map<string, double> v_count;
    v_count[beginSequence] = 0;  // get these into the table upfront into position 0 (and 1 if different)
    v_count[endSequence]   = 0;
#endif

    // process input line by line
    string str;
    vector<string> vstr;
    long long prevClsIdx = -1;
    string token;
    const string beginSequencePattern = beginSequence + " ";
    const string endSequencePattern   = " " + endSequence;
    while (getline(fp, str))
    {
        str.erase(0, str.find_first_not_of(' ')); // prefixing spaces
        str.erase(str.find_last_not_of(' ') + 1); // surfixing spaces

        if (!beginSequence.empty() && str.find(beginSequencePattern) == str.npos)
            str = beginSequencePattern + str;

        if (!endSequence.empty() && str.find(endSequencePattern) == str.npos)
            str = str + endSequencePattern;

        vstr = msra::strfun::split(str, "\t ");
        for (int i = 1; i < vstr.size(); i++)
            v_count[vstr[i]]++;
    }
    fp.close();

    std::cerr << "Vocabulary size " << v_count.size() << ".\n";

    std::vector<std::string> m_words;
    std::set<std::string> m_remained_words;
    std::unordered_map<std::string, size_t> m_index;

    std::vector<double> m_count;
    std::vector<int> m_class; // class index of each word

    typedef std::pair<std::string, double> stringdouble;
    std::priority_queue<stringdouble, std::vector<stringdouble>, compare_second<stringdouble>>
        q(compare_second<stringdouble>(), std::vector<stringdouble>(v_count.begin(), v_count.end()));

    size_t wordCountLessCutoff = v_count.size();
    if (cutoff > 0)
        for (const auto& iter : v_count)
        {
            if (iter.second <= cutoff)
                wordCountLessCutoff--;
        }
    if (wordCountLessCutoff <= 0)
        RuntimeError("No word remained after cutoff with threshold %d.", (int)cutoff);

    if (vocabSize > wordCountLessCutoff)
    {
        std::cerr << "Warning: actual vocabulary size is less than required." << endl;
        std::cerr << "\t\tRequired vocabulary size:" << vocabSize << endl;
        std::cerr << "\t\tActual vocabulary size:" << v_count.size() << endl;
        std::cerr << "\t\tActual vocabulary size after cutoff:" << wordCountLessCutoff << endl;
        std::cerr << "\t\tWe will change to actual vocabulary size: " << wordCountLessCutoff << endl;
        vocabSize = wordCountLessCutoff;
    }
    wrd2cls.Resize(vocabSize, 1);

    std::unordered_map<std::string, double> removed;
    double unkCount = 0;
    size_t size = 0;
    size_t actual_vocab_size = vocabSize - 1;
    while (size < actual_vocab_size && !q.empty())
    {
        size++;
        std::string word = q.top().first;
        double freq = q.top().second;
        if (word == unkWord)
        {
            unkCount += freq;
            actual_vocab_size++;
        }
        removed[q.top().first] = q.top().second;
        q.pop();
    }
    while (!q.empty())
    {
        unkCount += q.top().second;
        q.pop();
    }
    removed[unkWord] = unkCount;
    std::priority_queue<stringdouble, std::vector<stringdouble>, compare_second<stringdouble>>
        p(compare_second<stringdouble>(), std::vector<stringdouble>(removed.begin(), removed.end()));
    m_count.resize(removed.size());
    double total = 0;
    double dd = 0;
    if (nbrCls > 0)
    {
        for (const auto& iter : removed)
            total += iter.second;

        for (const auto& iter : removed)
            dd += sqrt(iter.second / total);
    }

    double df = 0;
    size_t class_id = 0;
    m_class.resize(p.size());

    while (!p.empty())
    {
        std::string word = p.top().first;
        double freq = p.top().second;
        if (nbrCls > 0)
        {
            df += sqrt(freq / total) / dd;
            if (df > 1)
                df = 1;

            if (df > 1.0 * (class_id + 1) / nbrCls && class_id < nbrCls)
                class_id++;
        }

        size_t wid = m_words.size();
        bool inserted = m_index.insert(make_pair(word, wid)).second;
        if (inserted)
            m_words.push_back(word);

        m_count[wid] = freq;
        if (nbrCls > 0)
            m_class[wid] = class_id;
        p.pop();
    }

    std::ofstream ofvocab;
    msra::files::make_intermediate_dirs(s2ws(outputVocabFile));
    ofvocab.open(outputVocabFile.c_str());
    for (size_t i = 0; i < m_index.size(); i++)
    {
        if (nbrCls > 0)
            wrd2cls(i, 0) = (ElemType) m_class[i];
        long long clsIdx = nbrCls > 0 ? m_class[i] : 0;
        if (nbrCls > 0 && clsIdx != prevClsIdx)
        {
            cls2idx(clsIdx, 0) = (ElemType) i; // the left boundary of clsIdx
            prevClsIdx = m_class[i];
        }
        ofvocab << "     " << i << "\t     " << m_count[i] << "\t" << m_words[i] << "\t" << clsIdx << std::endl;
    }
    ofvocab.close();
    std::cerr << "Created vocabulary file with " << v_count.size() << " entries.\n";

    if (nbrCls > 0)
    {
        // write the outputs
        // TODO: use safe-save, i.e. write to temp name and rename at the end
        msra::files::make_intermediate_dirs(s2ws(outputWord2Cls));
        ofstream owfp(outputWord2Cls.c_str());
        if (!owfp)
            RuntimeError("Failed to write to %s", outputWord2Cls.c_str());
        for (size_t r = 0; r < wrd2cls.GetNumRows(); r++)
            owfp << (int) wrd2cls(r, 0) << endl;
        owfp.close();
        std::cerr << "Created word-to-class map with " << wrd2cls.GetNumRows() << " entries.\n";

        msra::files::make_intermediate_dirs(s2ws(outputCls2Index));
        ofstream ocfp(outputCls2Index.c_str());
        if (!ocfp)
            RuntimeError("Failed to write to %s", outputCls2Index.c_str());
        for (size_t r = 0; r < cls2idx.GetNumRows(); r++)
            ocfp << (int) cls2idx(r, 0) << endl;
        ocfp.close();
        std::cerr << "Created class-to-index map with " << cls2idx.GetNumRows() << " entries.\n";
    }
}

template void DoWriteWordAndClassInfo<float>(const ConfigParameters& config);
template void DoWriteWordAndClassInfo<double>(const ConfigParameters& config);

// ===========================================================================
// DoTopologyPlot() - implements CNTK "plot" command
// ===========================================================================

// do topological plot of computation network
template <typename ElemType>
void DoTopologyPlot(const ConfigParameters& config)
{
    wstring modelPath = config(L"modelPath");
    wstring outdot = config(L"outputDotFile");  // filename for the dot language output, if not specified, %modelpath%.dot will be used
    wstring outRending = config(L"outputFile"); // filename for the rendered topology plot
    // this can be empty, in that case no rendering will be done
    // or if this is set, renderCmd must be set, so CNTK will call re
    wstring RenderCmd = config(L"RenderCmd"); // if this option is set, then CNTK will call the render to convert the outdotFile to a graph
    // e.g. "d:\Tools\graphviz\bin\dot.exe -Tpng -x <IN> -o<OUT>"
    //              where <IN> and <OUT> are two special placeholders

    // ========================================
    // Sec. 1 option check
    // ========================================
    if (outdot.empty())
    {
        outdot = modelPath + L".dot";
    }

    wstring rescmd;
    if (!outRending.empty()) // we need to render the plot
    {
        std::wregex inputPlaceHolder(L"(.+)(<IN>)(.*)");
        std::wregex outputPlaceHolder(L"(.+)(<OUT>)(.*)");

        rescmd = regex_replace(RenderCmd, inputPlaceHolder, L"$1" + outdot + L"$3");
        rescmd = regex_replace(rescmd, outputPlaceHolder, L"$1" + outRending + L"$3");
    }

    ComputationNetwork net(-1);
    net.Load<ElemType>(modelPath);
    net.PlotNetworkTopology(outdot);
    fprintf(stderr, "Output network description in dot language to %S\n", outdot.c_str());

    if (!outRending.empty())
    {
        fprintf(stderr, "Executing a third-part tool for rendering dot:\n%S\n", rescmd.c_str());
#ifdef __unix__
        const auto rc = system(msra::strfun::utf8(rescmd).c_str());
        rc /*ignoring the result--this gets flagged by gcc if we don't save the return value*/;
#else
        _wsystem(rescmd.c_str());
#endif
        fprintf(stderr, "Done\n");
    }
}

template void DoTopologyPlot<float>(const ConfigParameters& config);
template void DoTopologyPlot<double>(const ConfigParameters& config);
