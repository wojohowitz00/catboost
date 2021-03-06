#include "modes.h"
#include "cmd_line.h"
#include "proceed_pool_in_blocks.h"

#include <catboost/libs/algo/apply.h>
#include <catboost/libs/algo/plot.h>
#include <catboost/libs/data/load_data.h>
#include <catboost/libs/metrics/metric.h>

#include <util/system/fs.h>
#include <util/string/iterator.h>
#include <util/folder/tempdir.h>

struct TModeEvalMetricsParams {
    ui32 Step = 1;
    ui32 FirstIteration = 0;
    ui32 EndIteration = 0;
    int ReadBlockSize = 32768;
    TString MetricsDescription;
    TString ResultDirectory;
    TString TmpDir;

    void BindParserOpts(NLastGetopt::TOpts& parser) {
        parser.AddLongOption("ntree-start", "Start iteration.")
                .RequiredArgument("INT")
                .StoreResult(&FirstIteration);
        parser.AddLongOption("ntree-end", "End iteration.")
                .RequiredArgument("INT")
                .StoreResult(&EndIteration);
        parser.AddLongOption("eval-period", "Eval metrics every eval-period trees.")
                .RequiredArgument("INT")
                .StoreResult(&Step);
        parser.AddLongOption("metrics", "coma-separated eval metrics")
                .RequiredArgument("String")
                .StoreResult(&MetricsDescription);
        parser.AddLongOption("result-dir", "directory with results")
                .RequiredArgument("String")
                .StoreResult(&ResultDirectory);
        parser.AddLongOption("block-size", "Compute block size")
                .RequiredArgument("INT")
                .DefaultValue("32768")
                .StoreResult(&ReadBlockSize);
        parser.AddLongOption("tmp-dir", "Dir to store approx for non-additive metrics. Use \"-\" to generate directory.")
                .RequiredArgument("String")
                .DefaultValue("-")
                .StoreResult(&TmpDir);
    }
};

int mode_eval_metrics(int argc, const char* argv[]) {
    TAnalyticalModeCommonParams params;
    TModeEvalMetricsParams plotParams;
    bool verbose = false;

    auto parser = NLastGetopt::TOpts();
    parser.AddHelpOption();
    params.BindParserOpts(parser);
    plotParams.BindParserOpts(parser);
    parser.AddLongOption("verbose")
        .SetFlag(&verbose)
        .NoArgument();
    parser.SetFreeArgsNum(0);
    {
        NLastGetopt::TOptsParseResult parseResult(&parser, argc, argv);
        Y_UNUSED(parseResult);
    }
    if (verbose) {
        SetVerboseLogingMode();
    } else {
        SetSilentLogingMode();
    }

    CB_ENSURE(NFs::Exists(params.ModelFileName), "Model file doesn't exist " << params.ModelFileName);
    TFullModel model = ReadModel(params.ModelFileName);
    CB_ENSURE(model.ObliviousTrees.CatFeatures.empty() || !params.CdFile.empty(), "Model has categorical features. Specify column_description file with correct categorical features.");
    if (plotParams.EndIteration == 0) {
        plotParams.EndIteration = model.ObliviousTrees.TreeSizes.size();
    }
    if (plotParams.TmpDir == "-") {
        plotParams.TmpDir = TTempDir().Name();
    }
    // TODO(annaveronika): if AUC is specified, we will use a lot of disc for approxes on every iteration. Need to make a warning at least or decide if we load pool in memory.

    TVector<TString> metricsDescription;
    for (const auto& metricDescription : StringSplitter(plotParams.MetricsDescription).Split(',')) {
        metricsDescription.emplace_back(metricDescription.Token());
    }

    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(params.ThreadCount - 1);

    auto metrics = CreateMetricsFromDescription(metricsDescription, model.ObliviousTrees.ApproxDimension);
    TMetricsPlotCalcer plotCalcer = CreateMetricCalcer(
        model,
        plotParams.FirstIteration,
        plotParams.EndIteration,
        plotParams.Step,
        executor,
        plotParams.TmpDir,
        metrics
    );

    ReadAndProceedPoolInBlocks(params, plotParams.ReadBlockSize, [&](const TPool& poolPart) {
        plotCalcer.ProceedDataSet(poolPart);
    });

    plotCalcer.SaveResult(plotParams.ResultDirectory, params.OutputPath).ClearTempFiles();

    return 0;
}
