/*
 * This file is part of the PySide project.
 *
 * Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: PySide team <contact@pyside.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <QCoreApplication>
#include <QLinkedList>
#include <QLibrary>
#include <QDomDocument>
#include <QSharedMemory>
#include <QProcess>
#include <iostream>
#include <signal.h>
#include <apiextractor.h>
#include "generatorrunnerconfig.h"
#include "generator.h"

#ifdef _WINDOWS
    #define PATH_SPLITTER ";"
#else
    #define PATH_SPLITTER ":"
#endif

#define ARG_LENGTH    38
#define MAX_INSTANCES 10

enum {
    UPDATED,
    WAITING,
    ERROR
};

static void printOptions(QTextStream& s, const QMap<QString, QString>& options) {
    QMap<QString, QString>::const_iterator it = options.constBegin();
    s.setFieldAlignment(QTextStream::AlignLeft);
    for (; it != options.constEnd(); ++it) {
        s << "  --";
        s.setFieldWidth(ARG_LENGTH);
        s << it.key();

        s.setFieldWidth(ARG_LENGTH + 5); //include that "  --" at beginning
        if (it.key().length() > ARG_LENGTH)
            s << endl;

        int breaks = it.value().count("\n");
        if (breaks)
            for (int counter = 0; counter <= breaks; ++counter) {
                s << it.value().section("\n", counter, counter);
                if (counter < breaks)
                    s << endl;
            }
        else
            s << it.value();

        s.setFieldWidth(0);
        s << endl;
    }
}

typedef void (*getGeneratorsFunc)(QLinkedList<Generator*>*);

static bool processProjectFile(QFile& projectFile, QMap<QString, QString>& args)
{
    QByteArray line = projectFile.readLine().trimmed();
    if (line.isEmpty() || line != "[generator-project]")
        return false;

    QStringList includePaths;
    QStringList typesystemPaths;

    while (!projectFile.atEnd()) {
        line = projectFile.readLine().trimmed();
        if (line.isEmpty())
            continue;

        int split = line.indexOf("=");
        QString key;
        QString value;
        if (split > 0) {
            key = line.left(split - 1).trimmed();
            value = line.mid(split + 1).trimmed();
        } else {
            key = line;
        }

        if (key == "include-path")
            includePaths << QDir::toNativeSeparators(value);
        else if (key == "typesystem-path")
            typesystemPaths << QDir::toNativeSeparators(value);
        else if (key == "header-file")
            args["arg-1"] = value;
        else if (key == "typesystem-file")
            args["arg-2"] = value;
        else
            args[key] = value;
    }

    if (!includePaths.isEmpty())
        args["include-paths"] = includePaths.join(PATH_SPLITTER);

    if (!typesystemPaths.isEmpty())
        args["typesystem-paths"] = typesystemPaths.join(PATH_SPLITTER);

    return true;
}

static QMap<QString, QString> getInitializedArguments()
{
    QMap<QString, QString> args;
    QStringList arguments = QCoreApplication::arguments();
    QString appName = arguments.first();
    arguments.removeFirst();

    QString projectFileName;
    foreach (const QString& arg, arguments) {
        if (arg.startsWith("--project-file")) {
            int split = arg.indexOf("=");
            if (split > 0)
                projectFileName = arg.mid(split + 1).trimmed();
            break;
        }
    }

    if (projectFileName.isNull())
        return args;

    if (!QFile::exists(projectFileName)) {
        std::cerr << qPrintable(appName) << ": Project file \"";
        std::cerr << qPrintable(projectFileName) << "\" not found.";
        std::cerr << std::endl;
        return args;
    }

    QFile projectFile(projectFileName);
    if (!projectFile.open(QIODevice::ReadOnly))
        return args;

    if (!processProjectFile(projectFile, args)) {
        std::cerr << qPrintable(appName) << ": first line of project file \"";
        std::cerr << qPrintable(projectFileName) << "\" must be the string \"[generator-project]\"";
        std::cerr << std::endl;
        return args;
    }

    return args;
}

static QMap<QString, QString> getCommandLineArgs()
{
    QMap<QString, QString> args = getInitializedArguments();
    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();

    int argNum = 0;
    foreach (QString arg, arguments) {
        arg = arg.trimmed();
        if (arg.startsWith("--")) {
            int split = arg.indexOf("=");
            if (split > 0)
                args[arg.mid(2).left(split-2)] = arg.mid(split + 1).trimmed();
            else
                args[arg.mid(2)] = QString();
        } else if (arg.startsWith("-")) {
            args[arg.mid(1)] = QString();
        } else {
            argNum++;
            args[QString("arg-%1").arg(argNum)] = arg;
        }
    }
    return args;
}

void printUsage(const GeneratorList& generators)
{
    QTextStream s(stdout);
    s << "Usage:\n  "
    << "generator [options] header-file typesystem-file\n\n"
    "General options:\n";
    QMap<QString, QString> generalOptions;
    generalOptions.insert("project-file=<file>", "Text file containing a description of\nthe binding project. Replaces and\noverrides command line arguments");
    generalOptions.insert("debug-level=[sparse|medium|full]", "Set the debug level");
    generalOptions.insert("silent", "Avoid printing any message");
    generalOptions.insert("help", "Display this help and exit");
    generalOptions.insert("no-suppress-warnings", "Show all warnings");
    generalOptions.insert("output-directory=<path>", "The directory where the generated\nfiles will be written");
    generalOptions.insert("include-paths=<path>[" PATH_SPLITTER "<path>" PATH_SPLITTER "...]", "Include paths used by the C++ parser");
    generalOptions.insert("typesystem-paths=<path>[" PATH_SPLITTER "<path>" PATH_SPLITTER "...]", "Paths used when searching for\ntypesystems");
    generalOptions.insert("documentation-only", "Do not generates any code, just the\ndocumentation");
    generalOptions.insert("license-file=<license-file>", "File used for copyright headers of\ngenerated files");
    generalOptions.insert("version", "Output version information and exit");
    generalOptions.insert("generator-set=<\"generator module\">", "generator-set to be used. e.g. qtdoc");
    generalOptions.insert("api-version=<\"version\">", "Specify the supported api version used\nto generate the bindings");
    generalOptions.insert("drop-type-entries=\"<TypeEntry0>[;TypeEntry1;...]\"", "Semicolon separated list of typesystem\nentries (classes, namespaces, global\nfunctions and enums) to be dropped\nfrom generation.");
    generalOptions.insert("max-instances=<\"max\">", "Limits the number of generatorrunner\ninstances that can be called at the\nsame time");
    printOptions(s, generalOptions);

    foreach (Generator* generator, generators) {
        QMap<QString, QString> options = generator->options();
        if (!options.isEmpty()) {
            s << endl << generator->name() << " options:\n";
            printOptions(s, generator->options());
        }
    }
}

int checkInstances(QSharedMemory &instControl, qint64 currentProc) {
    qint64 *instControlData;

    instControlData = (qint64 *)instControl.data();
    for (int counter = 1; counter <= *instControlData; counter++)
        if (kill(*(instControlData + counter), 0)) {
            *(instControlData + counter) = currentProc;
            return UPDATED;
        }
    return WAITING;
}

int instControlUpdate(const int maxInst, QSharedMemory &instControl, bool clean) {
    qint64 *instControlData;
    int instances = 1;

    instControlData = (qint64 *)instControl.data();

    qint64 currentProc = QCoreApplication::applicationPid();
    qDebug() << "luck:" << currentProc;

    if (clean) {
        *instControlData = 1;
        *(instControlData + 1) = currentProc;

        //qDebug() << "indo dormir!";
        //sleep(5);

        return UPDATED;
//    } else
//        sleep(3);
    }

    instances = *instControlData;
    qDebug() << "proc" << instances << ":" << *(instControlData + instances);

    if (instances < maxInst) {
        *instControlData = ++instances;
        *(instControlData + instances) = currentProc;
    } else {
        return checkInstances(instControl, currentProc);
    }
    qDebug() << "instances:" << instances;
    return UPDATED;
}

int limitInstances(QString maxInstancesStr, QString key) {
    bool ok, clean = true;
    const int maxInst = maxInstancesStr.toInt(&ok);

    if (!ok || maxInst > MAX_INSTANCES)
        return EXIT_FAILURE;

    qDebug() << "key:" << key;
    QSharedMemory instControl(key);

    if (!instControl.create(sizeof(qint64) * (MAX_INSTANCES + 1))) {
        clean = false;
        if (instControl.error() != QSharedMemory::AlreadyExists)
            qDebug() << "create:" << instControl.errorString() << instControl.error();
    }

    if (!instControl.isAttached())
        if (!instControl.attach())
            qDebug() << "attach:" << instControl.errorString() << instControl.error();

    /*Unfortunately the lock() function doesn't work as expected.
      The code is safe enough to be used until Qt people provide
      a bug fix for: https://bugreports.qt.nokia.com//browse/QTBUG-10364 */
    if (!instControl.lock())
        qDebug() << "lock:" << instControl.errorString();

    qDebug() << "control update";

    while (instControlUpdate(maxInst, instControl, clean) == WAITING)
        sleep(10);

    instControl.unlock();
    qDebug() << "memoria unlocada";
    instControl.detach();

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    // needed by qxmlpatterns
    QCoreApplication app(argc, argv);

    // Store command arguments in a map
    QMap<QString, QString> args = getCommandLineArgs();
    GeneratorList generators;

    if (args.contains("version")) {
        std::cout << "generatorrunner v" GENERATORRUNNER_VERSION << std::endl;
        std::cout << "Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies)" << std::endl;
        return EXIT_SUCCESS;
    }

    // Try to load a generator
    QString generatorSet = args.value("generator-set");

    // Also check "generatorSet" command line argument for backward compatibility.
    if (generatorSet.isEmpty())
        generatorSet = args.value("generatorSet");

    if (!generatorSet.isEmpty()) {
        QFileInfo generatorFile(generatorSet);

        if (!generatorFile.exists()) {
            QString generatorSetName(generatorSet + "_generator" + MODULE_EXTENSION);

            // More library paths may be added via the QT_PLUGIN_PATH environment variable.
            QCoreApplication::addLibraryPath(GENERATORRUNNER_PLUGIN_DIR);
            foreach (const QString& path, QCoreApplication::libraryPaths()) {
                generatorFile.setFile(QDir(path), generatorSetName);
                if (generatorFile.exists())
                    break;
            }
        }

        if (!generatorFile.exists()) {
            std::cerr << argv[0] << ": Error loading generator-set plugin: ";
            std::cerr << qPrintable(generatorFile.baseName()) << " module not found." << std::endl;
            return EXIT_FAILURE;
        }

        QLibrary plugin(generatorFile.filePath());
        getGeneratorsFunc getGenerators = (getGeneratorsFunc)plugin.resolve("getGenerators");
        if (getGenerators) {
            getGenerators(&generators);
        } else {
            std::cerr << argv[0] << ": Error loading generator-set plugin: " << qPrintable(plugin.errorString()) << std::endl;
            return EXIT_FAILURE;
        }
    } else if (!args.contains("help")) {
        std::cerr << argv[0] << ": You need to specify a generator with --generator-set=GENERATOR_NAME" << std::endl;
        return EXIT_FAILURE;
    }

    if (args.contains("help")) {
        printUsage(generators);
        return EXIT_SUCCESS;
    }


    QString licenseComment;
    if (args.contains("license-file") && !args.value("license-file").isEmpty()) {
        QString licenseFileName = args.value("license-file");
        if (QFile::exists(licenseFileName)) {
            QFile licenseFile(licenseFileName);
            if (licenseFile.open(QIODevice::ReadOnly))
                licenseComment = licenseFile.readAll();
        } else {
            std::cerr << "Couldn't find the file containing the license heading: ";
            std::cerr << qPrintable(licenseFileName) << std::endl;
            return EXIT_FAILURE;
        }
    }

    QString outputDirectory = args.contains("output-directory") ? args["output-directory"] : "out";
    if (!QDir(outputDirectory).exists()) {
        if (!QDir().mkpath(outputDirectory)) {
            ReportHandler::warning("Can't create output directory: "+outputDirectory);
            return EXIT_FAILURE;
        }
    }
    // Create and set-up API Extractor
    ApiExtractor extractor;
    extractor.setLogDirectory(outputDirectory);

    if (args.contains("silent")) {
        extractor.setSilent(true);
    } else if (args.contains("debug-level")) {
        QString level = args.value("debug-level");
        if (level == "sparse")
            extractor.setDebugLevel(ReportHandler::SparseDebug);
        else if (level == "medium")
            extractor.setDebugLevel(ReportHandler::MediumDebug);
        else if (level == "full")
            extractor.setDebugLevel(ReportHandler::FullDebug);
    }
    if (args.contains("no-suppress-warnings"))
        extractor.setSuppressWarnings(false);

    if (args.contains("api-version"))
        extractor.setApiVersion(args["api-version"].toDouble());

    if (args.contains("drop-type-entries"))
        extractor.setDropTypeEntries(args["drop-type-entries"]);

    if (args.contains("typesystem-paths"))
        extractor.addTypesystemSearchPath(args.value("typesystem-paths").split(PATH_SPLITTER));
    if (!args.value("include-paths").isEmpty())
        extractor.addIncludePath(args.value("include-paths").split(PATH_SPLITTER));

    if (args.contains("max-instances"))
        if (limitInstances(args["max-instances"], "testing") == EXIT_FAILURE) {
            std::cerr << argv[0] << ": You must set max-instances using numbers [0 - " << MAX_INSTANCES << "]" << std::endl;
            return EXIT_FAILURE;
        }

    QString cppFileName = args.value("arg-1");
    QString typeSystemFileName = args.value("arg-2");
    if (args.contains("arg-3")) {
        std::cerr << "Too many arguments!" << std::endl;
        return EXIT_FAILURE;
    }
    extractor.setCppFileName(cppFileName);
    extractor.setTypeSystem(typeSystemFileName);
    if (!extractor.run())
        return EXIT_FAILURE;

    if (!extractor.classCount())
        ReportHandler::warning("No C++ classes found!");

    foreach (Generator* g, generators) {
        g->setOutputDirectory(outputDirectory);
        g->setLicenseComment(licenseComment);
        if (g->setup(extractor, args))
            g->generate();
    }
    qDeleteAll(generators);

    ReportHandler::flush();
    std::cout << "Done, " << ReportHandler::warningCount();
    std::cout << " warnings (" << ReportHandler::suppressedCount() << " known issues)";
    std::cout << std::endl;
}
