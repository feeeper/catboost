#include "catboost_logger_helpers.h"


TString TOutputFiles::AlignFilePathAndCreateDir(const TString& baseDir, const TString& fileName, const TString& namePrefix) {
    TFsPath filePath(fileName);
    if (filePath.IsAbsolute()) {
        return JoinFsPaths(filePath.Dirname(), namePrefix + filePath.Basename());
    }
    TString result = JoinFsPaths(baseDir, namePrefix + fileName);

    TFsPath resultingPath(result);
    auto dirName = resultingPath.Dirname();
    TFsPath dirPath(dirName);
    if (!dirName.empty() && !dirPath.Exists()) {
        dirPath.MkDirs();
    }
    return result;
}

void TOutputFiles::InitializeFiles(const NCatboostOptions::TOutputFilesOptions& params, const TString& namesPrefix) {
    if (!params.AllowWriteFiles()) {
        Y_ASSERT(TimeLeftLogFile.empty());
        Y_ASSERT(LearnErrorLogFile.empty());
        Y_ASSERT(TestErrorLogFile.empty());
        Y_ASSERT(MetaFile.empty());
        Y_ASSERT(SnapshotFile.empty());
        return;
    }

    const auto& trainDir = params.GetTrainDir();
    TFsPath trainDirPath(trainDir);
    if (!trainDir.empty() && !trainDirPath.Exists()) {
        trainDirPath.MkDirs();
    }
    NamesPrefix = namesPrefix;
    CB_ENSURE(!params.GetTimeLeftLogFilename().empty(), "empty time_left filename");
    TimeLeftLogFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, params.GetTimeLeftLogFilename(), NamesPrefix);

    CB_ENSURE(!params.GetLearnErrorFilename().empty(), "empty learn_error filename");
    LearnErrorLogFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, params.GetLearnErrorFilename(), NamesPrefix);
    if (params.GetTestErrorFilename()) {
        TestErrorLogFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, params.GetTestErrorFilename(), NamesPrefix);
    }
    if (params.SaveSnapshot()) {
        SnapshotFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, params.GetSnapshotFilename(), NamesPrefix);
    }
    const TString& metaFileFilename = params.GetMetaFileFilename();
    CB_ENSURE(!metaFileFilename.empty(), "empty meta filename");
    MetaFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, metaFileFilename, NamesPrefix);

    const TString& jsonLogFilename = params.GetJsonLogFilename();
    CB_ENSURE(!jsonLogFilename.empty(), "empty json_log filename");
    JsonLogFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, jsonLogFilename, "");

    const TString& profileLogFilename = params.GetProfileLogFilename();
    CB_ENSURE(!profileLogFilename.empty(), "empty profile_log filename");
    ProfileLogFile = TOutputFiles::AlignFilePathAndCreateDir(trainDir, profileLogFilename, "");

    ExperimentName = params.GetName();
    TrainDir = trainDir;
}

void WriteHistory(
        const TVector<TString>& metricsDescription,
        const TMetricsAndTimeLeftHistory& history,
        const TString& learnToken,
        const TVector<const TString>& testTokens,
        TLogger* logger
) {
    for (int iteration = 0; iteration < history.TimeHistory.ysize(); ++iteration) {
        TOneInterationLogger oneIterLogger(*logger);
        if (iteration < history.LearnMetricsHistory.ysize()) {
            const auto& learnMetrics =  history.LearnMetricsHistory[iteration];
            for (int i = 0; i < learnMetrics.ysize(); ++i) {
                oneIterLogger.OutputMetric(learnToken, TMetricEvalResult(metricsDescription[i], learnMetrics[i], i == 0));
            }
        }
        if (iteration < history.TestMetricsHistory.ysize()) {
            const int testCount = history.TestMetricsHistory[0].ysize();
            for (int testIdx = 0; testIdx < testCount; ++testIdx) {
                const auto& testMetrics = history.TestMetricsHistory[iteration][testIdx];
                for (int i = 0; i < testMetrics.ysize(); ++i) {
                    oneIterLogger.OutputMetric(testTokens[testIdx], TMetricEvalResult(metricsDescription[i], testMetrics[i], i == 0));
                }
            }
        }
        oneIterLogger.OutputProfile(TProfileResults(history.TimeHistory[iteration].PassedTime, history.TimeHistory[iteration].RemainingTime));
    }
}

void AddFileLoggers(
        bool detailedProfile,
        const TString& learnErrorLogFile,
        const TString& testErrorLogFile,
        const TString& timeLogFile,
        const TString& jsonLogFile,
        const TString& profileLogFile,
        const TString& trainDir,
        const NJson::TJsonValue& metaJson,
        int metricPeriod,
        TLogger* logger
) {
    TIntrusivePtr<ILoggingBackend> jsonLoggingBackend = new TJsonLoggingBackend(jsonLogFile, metaJson, metricPeriod);
    for (auto& jsonToken : metaJson["learn_sets"].GetArraySafe()) {
        TString token = jsonToken.GetString();
        logger->AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TErrorFileLoggingBackend(learnErrorLogFile)));
        logger->AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TTensorBoardLoggingBackend(JoinFsPaths(trainDir, token))));
        logger->AddBackend(token, jsonLoggingBackend);
    }
    TIntrusivePtr<ILoggingBackend> errorFileLoggingBackend;
    for (auto& jsonToken : metaJson["test_sets"].GetArraySafe()) {
        TString token = jsonToken.GetString();
        if (!errorFileLoggingBackend) {
            errorFileLoggingBackend.Reset(new TErrorFileLoggingBackend(testErrorLogFile));
        }
        logger->AddBackend(token, errorFileLoggingBackend);
        logger->AddBackend(token, TIntrusivePtr<ILoggingBackend>(new TTensorBoardLoggingBackend(JoinFsPaths(trainDir, token))));
        logger->AddBackend(token, jsonLoggingBackend);
    }
    logger->AddProfileBackend(TIntrusivePtr<ILoggingBackend>(new TTimeFileLoggingBackend(timeLogFile)));
    logger->AddProfileBackend(jsonLoggingBackend);
    if (detailedProfile) {
        logger->AddProfileBackend(TIntrusivePtr<ILoggingBackend>(new TProfileLoggingBackend(profileLogFile)));
        logger->AddProfileBackend(TIntrusivePtr<ILoggingBackend>(new TJsonProfileLoggingBackend(profileLogFile + ".json")));
    }
}

void AddConsoleLogger(
        const TString& learnToken,
        const TVector<const TString>& testTokens,
        bool hasTrain,
        int metricPeriod,
        int iterationCount,
        TLogger* logger
) {
    TIntrusivePtr<ILoggingBackend> consoleLoggingBackend = new TConsoleLoggingBackend(/*detailedProfile=*/false, metricPeriod, iterationCount);
    if (hasTrain) {
        logger->AddBackend(learnToken, consoleLoggingBackend);
    }
    for (const TString& testToken : testTokens) {
        logger->AddBackend(testToken, consoleLoggingBackend);
    }
    logger->AddProfileBackend(consoleLoggingBackend);
}

void Log(
        const TVector<TString>& metricsDescription,
        const TVector<bool>& skipMetricOnTrain,
        const TVector<TVector<double>>& learnErrorsHistory,
        const TVector<TVector<TVector<double>>>& testErrorsHistory, // [iter][test][metric]
        double bestErrorValue,
        int bestIteration,
        const TProfileResults& profileResults,
        const TString& learnToken,
        const TVector<const TString>& testTokens,
        bool outputErrors,
        TLogger* logger
) {
    TOneInterationLogger oneIterLogger(*logger);
    int iteration = profileResults.PassedIterations - 1;
    if (outputErrors && iteration < learnErrorsHistory.ysize()) {
        const TVector<double>& learnErrors = learnErrorsHistory[iteration];
        size_t metricIdx = 0;
        for (int i = 0; i < learnErrors.ysize(); ++i) {
            while (skipMetricOnTrain[metricIdx]) {
                ++metricIdx;
            }
            oneIterLogger.OutputMetric(learnToken, TMetricEvalResult(metricsDescription[metricIdx], learnErrors[i], metricIdx == 0));
            ++metricIdx;
        }
    }
    if (outputErrors && iteration < testErrorsHistory.ysize()) {
        const int testCount = testErrorsHistory[iteration].ysize();
        for (int testIdx = 0; testIdx < testCount; ++testIdx) {
            const int metricCount = testErrorsHistory[iteration][testIdx].ysize();
            for (int metricIdx = 0; metricIdx < metricCount; ++metricIdx) {
                double testError = testErrorsHistory[iteration][testIdx][metricIdx];
                bool isMainMetric = metricIdx == 0;
                const TString& token = testTokens[testIdx];
                if (testIdx == 0) {
                    // Only test 0 should be followed by 'best:'
                    oneIterLogger.OutputMetric(token, TMetricEvalResult(metricsDescription[metricIdx], testError, bestErrorValue, bestIteration, isMainMetric));
                } else {
                    oneIterLogger.OutputMetric(token, TMetricEvalResult(metricsDescription[metricIdx] + ":" + ToString(testIdx), testError, isMainMetric));
                }
            }
        }
    }

    oneIterLogger.OutputProfile(profileResults);
}


NJson::TJsonValue GetJsonMeta(
        int iterationCount,
        const TString& optionalExperimentName,
        const TVector<const IMetric*>& metrics,
        const TVector<TString>& learnSetNames,
        const TVector<TString>& testSetNames,
        ELaunchMode launchMode
) {
    NJson::TJsonValue meta;
    meta["iteration_count"] = iterationCount;
    meta["name"] = optionalExperimentName;

    meta.InsertValue("learn_sets", NJson::JSON_ARRAY);
    for (auto& name : learnSetNames) {
        meta["learn_sets"].AppendValue(name);
    }

    meta.InsertValue("test_sets", NJson::JSON_ARRAY);
    for (auto& name : testSetNames) {
        meta["test_sets"].AppendValue(name);
    }

    meta.InsertValue("learn_metrics", NJson::JSON_ARRAY);
    meta.InsertValue("test_metrics", NJson::JSON_ARRAY);
    for (const auto& loss : metrics) {

        NJson::TJsonValue metricJson;
        metricJson.InsertValue("name", loss->GetDescription());

        EMetricBestValue bestValueType;
        float bestValue;
        loss->GetBestValue(&bestValueType, &bestValue);
        TString bestValueString;
        if (bestValueType != EMetricBestValue::FixedValue) {
            metricJson.InsertValue("best_value", ToString(bestValueType));
        } else {
            metricJson.InsertValue("best_value", bestValue);
        }

        const TMap<TString, TString>& hints = loss->GetHints();
        if (!learnSetNames.empty() && (!hints.has("skip_train") || hints.at("skip_train") == "false")) {
            meta["learn_metrics"].AppendValue(metricJson);
        }
        if (!testSetNames.empty()) {
            meta["test_metrics"].AppendValue(metricJson);
        }
    }

    meta.InsertValue("launch_mode", ToString<ELaunchMode>(launchMode));
    return meta;
}


TString GetTrainModelLearnToken() {
    return "learn";
}

TVector<const TString> GetTrainModelTestTokens(int testCount) {
    TString testTokenPrefix = "test";
    TVector<const TString> testTokens;
    for (int testIdx = 0; testIdx < testCount; ++testIdx) {
        TString testToken = testTokenPrefix + (testIdx > 0 ? ToString(testIdx) : "");
        testTokens.push_back(testToken);
    }
    return testTokens;
}


void InitializeFileLoggers(
        const NCatboostOptions::TCatBoostOptions& catboostOptions,
        const TOutputFiles& outputFiles,
        const TVector<const IMetric*>& metrics,
        const TString& learnToken,
        const TVector<const TString>& testTokens,
        int metricPeriod,
        TLogger* logger) {
    TVector<TString> metricDescriptions = GetMetricsDescription(metrics);

    TVector<TString> learnSetNames = {outputFiles.NamesPrefix + learnToken};
    TVector<TString> testSetNames;
    for (int testIdx = 0; testIdx < testTokens.ysize(); ++testIdx) {
        testSetNames.push_back({outputFiles.NamesPrefix + testTokens[testIdx]});
    }

    AddFileLoggers(
            catboostOptions.IsProfile,
            outputFiles.LearnErrorLogFile,
            outputFiles.TestErrorLogFile,
            outputFiles.TimeLeftLogFile,
            outputFiles.JsonLogFile,
            outputFiles.ProfileLogFile,
            outputFiles.TrainDir,
            GetJsonMeta(
                    catboostOptions.BoostingOptions->IterationCount.Get(),
                    outputFiles.ExperimentName,
                    metrics,
                    learnSetNames,
                    testSetNames,
                    ELaunchMode::Train),
            metricPeriod,
            logger
    );


}

