/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *  Copyright (C) 2009 Thomas Zander <zander@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "repository.h"
#include "CommandLineParser.h"
#include <QTextStream>
#include <QDebug>
#include <QDir>
#include <QLinkedList>

static const int maxSimultaneousProcesses = 100;

static const int maxMark = (1 << 20) - 1; // some versions of git-fast-import are buggy for larger values of maxMark

class FastImportRepository : public Repository
{
public:
    class Transaction : public Repository::Transaction
    {
        Q_DISABLE_COPY(Transaction)
        friend class FastImportRepository;

        FastImportRepository *repository;
        QByteArray branch;
        QByteArray svnprefix;
        QByteArray author;
        QByteArray log;
        uint datetime;
        int revnum;

        QVector<int> merges;

        QStringList deletedFiles;
        QByteArray modifiedFiles;

        inline Transaction() {}
    public:
        ~Transaction();
        void commit();

        void setAuthor(const QByteArray &author);
        void setDateTime(uint dt);
        void setLog(const QByteArray &log);

        void noteCopyFromBranch (const QString &prevbranch, int revFrom);

        void deleteFile(const QString &path);
        QIODevice *addFile(const QString &path, int mode, qint64 length);
    };
    FastImportRepository(const Rules::Repository &rule);
    int setupIncremental(int &cutoff);
    void restoreLog();
    ~FastImportRepository();

    void reloadBranches();
    int createBranch(const QString &branch, int revnum,
                     const QString &branchFrom, int revFrom);
    int deleteBranch(const QString &branch, int revnum);
    Repository::Transaction *newTransaction(const QString &branch, const QString &svnprefix, int revnum);

    void createAnnotatedTag(const QString &name, const QString &svnprefix, int revnum,
                            const QByteArray &author, uint dt,
                            const QByteArray &log);
    void finalizeTags();

private:
    struct Branch
    {
        int created;
        QVector<int> commits;
        QVector<int> marks;
    };
    struct AnnotatedTag
    {
        QString supportingRef;
        QByteArray svnprefix;
        QByteArray author;
        QByteArray log;
        uint dt;
        int revnum;
    };

    QHash<QString, Branch> branches;
    QHash<QString, AnnotatedTag> annotatedTags;
    QString name;
    QProcess fastImport;
    int commitCount;
    int outstandingTransactions;

    /* starts at 0, and counts up.  */
    int last_commit_mark;

    /* starts at maxMark and counts down. Reset after each SVN revision */
    int next_file_mark;

    bool processHasStarted;

    void startFastImport();
    void closeFastImport();

    // called when a transaction is deleted
    void forgetTransaction(Transaction *t);

    int resetBranch(const QString &branch, int revnum, int mark, const QByteArray &resetTo, const QByteArray &comment);
    int markFrom(const QString &branchFrom, int branchRevNum, QByteArray &desc);

    friend class ProcessCache;
    Q_DISABLE_COPY(FastImportRepository)
};

class PrefixingRepository : public Repository
{
    Repository *repo;
    QString prefix;
public:
    class Transaction : public Repository::Transaction
    {
        Q_DISABLE_COPY(Transaction)

        Repository::Transaction *txn;
        QString prefix;
    public:
        Transaction(Repository::Transaction *t, const QString &p) : txn(t), prefix(p) {}
        ~Transaction() { delete txn; }
        void commit() { txn->commit(); }

        void setAuthor(const QByteArray &author) { txn->setAuthor(author); }
        void setDateTime(uint dt) { txn->setDateTime(dt); }
        void setLog(const QByteArray &log) { txn->setLog(log); }

        void noteCopyFromBranch (const QString &prevbranch, int revFrom)
        { txn->noteCopyFromBranch(prevbranch, revFrom); }

        void deleteFile(const QString &path) { txn->deleteFile(prefix + path); }
        QIODevice *addFile(const QString &path, int mode, qint64 length)
        { return txn->addFile(prefix + path, mode, length); }
    };

    PrefixingRepository(Repository *r, const QString &p) : repo(r), prefix(p) {}

    int setupIncremental(int &) { return 1; }
    void restoreLog() {}

    int createBranch(const QString &branch, int revnum,
                     const QString &branchFrom, int revFrom)
    { return repo->createBranch(branch, revnum, branchFrom, revFrom); }

    int deleteBranch(const QString &branch, int revnum)
    { return repo->deleteBranch(branch, revnum); }

    Repository::Transaction *newTransaction(const QString &branch, const QString &svnprefix, int revnum)
    {
        Repository::Transaction *t = repo->newTransaction(branch, svnprefix, revnum);
        return new Transaction(t, prefix);
    }

    void createAnnotatedTag(const QString &name, const QString &svnprefix, int revnum,
                            const QByteArray &author, uint dt,
                            const QByteArray &log)
    { repo->createAnnotatedTag(name, svnprefix, revnum, author, dt, log); }
    void finalizeTags() { /* loop that called this will invoke it on 'repo' too */ }
};

class ProcessCache: QLinkedList<FastImportRepository *>
{
public:
    void touch(FastImportRepository *repo)
    {
        remove(repo);

        // if the cache is too big, remove from the front
        while (size() >= maxSimultaneousProcesses)
            takeFirst()->closeFastImport();

        // append to the end
        append(repo);
    }

    inline void remove(FastImportRepository *repo)
    {
#if QT_VERSION >= 0x040400
        removeOne(repo);
#else
        removeAll(repo);
#endif
    }
};
static ProcessCache processCache;

Repository *makeRepository(const Rules::Repository &rule, const QHash<QString, Repository *> &repositories)
{
    if (rule.forwardTo.isEmpty())
        return new FastImportRepository(rule);
    Repository *r = repositories[rule.forwardTo];
    if (!r) {
        qCritical() << "no repository with name" << rule.forwardTo << "found at line" << rule.lineNumber;
        return r;
    }
    return new PrefixingRepository(r, rule.prefix);
}

static QString marksFileName(QString name)
{
    name.replace('/', '_');
    name.prepend("marks-");
    return name;
}

FastImportRepository::FastImportRepository(const Rules::Repository &rule)
    : name(rule.name), commitCount(0), outstandingTransactions(0),  last_commit_mark(0), next_file_mark(maxMark), processHasStarted(false)
{
    foreach (Rules::Repository::Branch branchRule, rule.branches) {
        Branch branch;
        branch.created = 0;     // not created

        branches.insert(branchRule.name, branch);
    }

    // create the default branch
    branches["master"].created = 1;

    fastImport.setWorkingDirectory(name);
    if (!CommandLineParser::instance()->contains("dry-run")) {
        if (!QDir(name).exists()) { // repo doesn't exist yet.
            qDebug() << "Creating new repository" << name;
            QDir::current().mkpath(name);
            QProcess init;
            init.setWorkingDirectory(name);
            init.start("git", QStringList() << "--bare" << "init");
            init.waitForFinished(-1);
            {
                QFile marks(name + "/" + marksFileName(name));
                marks.open(QIODevice::WriteOnly);
                marks.close();
            }
        }
    }
}

static QString logFileName(QString name)
{
    name.replace('/', '_');
    name.prepend("log-");
    return name;
}

static int lastValidMark(QString name)
{
    QFile marksfile(name + "/" + marksFileName(name));
    if (!marksfile.open(QIODevice::ReadOnly))
        return 0;

    int prev_mark = 0;

    int lineno = 0;
    while (!marksfile.atEnd()) {
        QString line = marksfile.readLine();
        ++lineno;
        if (line.isEmpty())
            continue;

        int mark = 0;
        if (line[0] == ':') {
            int sp = line.indexOf(' ');
            if (sp != -1) {
                QString m = line.mid(1, sp-1);
                mark = m.toInt();
            }
        }

        if (!mark) {
            qCritical() << marksfile.fileName() << "line" << lineno << "marks file corrupt?";
            return 0;
        }

        if (mark == prev_mark) {
            qCritical() << marksfile.fileName() << "line" << lineno << "marks file has duplicates";
            return 0;
        }

        if (mark < prev_mark) {
            qCritical() << marksfile.fileName() << "line" << lineno << "marks file not sorted";
            return 0;
        }

        if (mark > prev_mark + 1)
            break;

        prev_mark = mark;
    }

    return prev_mark;
}

int FastImportRepository::setupIncremental(int &cutoff)
{
    QFile logfile(logFileName(name));
    if (!logfile.exists())
        return 1;

    logfile.open(QIODevice::ReadWrite);

    QRegExp progress("progress SVN r(\\d+) branch (.*) = :(\\d+)");

    int last_valid_mark = lastValidMark(name);

    int last_revnum = 0;
    qint64 pos = 0;
    int retval = 0;
    QString bkup = logfile.fileName() + ".old";

    while (!logfile.atEnd()) {
        pos = logfile.pos();
        QByteArray line = logfile.readLine();
        int hash = line.indexOf('#');
        if (hash != -1)
            line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (!progress.exactMatch(line))
            continue;

        int revnum = progress.cap(1).toInt();
        QString branch = progress.cap(2);
        int mark = progress.cap(3).toInt();

        if (revnum >= cutoff)
            goto beyond_cutoff;

        if (revnum < last_revnum)
            qWarning() << name << "revision numbers are not monotonic: "
                       << "got" << QString::number(last_revnum)
                       << "and then" << QString::number(revnum);

        if (mark > last_valid_mark) {
            qWarning() << name << "unknown commit mark found: rewinding -- did you hit Ctrl-C?";
            cutoff = revnum;
            goto beyond_cutoff;
        }

        last_revnum = revnum;

        if (last_commit_mark < mark)
            last_commit_mark = mark;

        Branch &br = branches[branch];
        if (!br.created || !mark || br.marks.isEmpty())
            br.created = revnum;
        br.commits.append(revnum);
        br.marks.append(mark);
    }

    retval = last_revnum + 1;
    if (retval == cutoff)
        /*
         * If a stale backup file exists already, remove it, so that
         * we don't confuse ourselves in 'restoreLog()'
         */
        QFile::remove(bkup);

    return retval;

  beyond_cutoff:
    // backup file, since we'll truncate
    QFile::remove(bkup);
    logfile.copy(bkup);

    // truncate, so that we ignore the rest of the revisions
    qDebug() << name << "truncating history to revision" << cutoff;
    logfile.resize(pos);
    return cutoff;
}

void FastImportRepository::restoreLog()
{
    QString file = logFileName(name);
    QString bkup = file + ".old";
    if (!QFile::exists(bkup))
        return;
    QFile::remove(file);
    QFile::rename(bkup, file);
}

FastImportRepository::~FastImportRepository()
{
    Q_ASSERT(outstandingTransactions == 0);
    closeFastImport();
}

void FastImportRepository::closeFastImport()
{
    if (fastImport.state() != QProcess::NotRunning) {
        fastImport.write("checkpoint\n");
        fastImport.waitForBytesWritten(-1);
        fastImport.closeWriteChannel();
        if (!fastImport.waitForFinished()) {
            fastImport.terminate();
            if (!fastImport.waitForFinished(200))
                qWarning() << "git-fast-import for repository" << name << "did not die";
        }
    }
    processHasStarted = false;
    processCache.remove(this);
}

void FastImportRepository::reloadBranches()
{
    foreach (QString branch, branches.keys()) {
        Branch &br = branches[branch];

        if (!br.marks.count() || !br.marks.last())
            continue;

        QByteArray branchRef = branch.toUtf8();
        if (!branchRef.startsWith("refs/"))
            branchRef.prepend("refs/heads/");

        fastImport.write("reset " + branchRef +
                        "\nfrom :" + QByteArray::number(br.marks.last()) + "\n\n"
                        "progress Branch " + branchRef + " reloaded\n");
    }
}

int FastImportRepository::markFrom(const QString &branchFrom, int branchRevNum, QByteArray &branchFromDesc)
{
    Branch &brFrom = branches[branchFrom];
    if (!brFrom.created)
        return -1;

    if (brFrom.commits.isEmpty()) {
        return -1;
    }
    if (branchRevNum == brFrom.commits.last()) {
        return brFrom.marks.last();
    }

    QVector<int>::const_iterator it = qUpperBound(brFrom.commits, branchRevNum);
    if (it == brFrom.commits.begin()) {
        return 0;
    }

    int closestCommit = *--it;

    if (!branchFromDesc.isEmpty()) {
        branchFromDesc += " at r" + QByteArray::number(branchRevNum);
        if (closestCommit != branchRevNum) {
            branchFromDesc += " => r" + QByteArray::number(closestCommit);
        }
    }

    return brFrom.marks[it - brFrom.commits.begin()];
}

int FastImportRepository::createBranch(const QString &branch, int revnum,
                                     const QString &branchFrom, int branchRevNum)
{
    startFastImport();

    QByteArray branchFromDesc = "from branch " + branchFrom.toUtf8();
    int mark = markFrom(branchFrom, branchRevNum, branchFromDesc);

    if (mark == -1) {
        qCritical() << branch << "in repository" << name
                    << "is branching from branch" << branchFrom
                    << "but the latter doesn't exist. Can't continue.";
        return EXIT_FAILURE;
    }

    QByteArray branchFromRef = ":" + QByteArray::number(mark);
    if (!mark) {
        qWarning() << branch << "in repository" << name << "is branching but no exported commits exist in repository"
                << "creating an empty branch.";
        branchFromRef = branchFrom.toUtf8();
        if (!branchFromRef.startsWith("refs/"))
            branchFromRef.prepend("refs/heads/");
        branchFromDesc += ", deleted/unknown";
    }

    qDebug() << "Creating branch:" << branch << "from" << branchFrom << "(" << branchRevNum << branchFromDesc << ")";

    return resetBranch(branch, revnum, mark, branchFromRef, branchFromDesc);
}

int FastImportRepository::deleteBranch(const QString &branch, int revnum)
{
    startFastImport();

    static QByteArray null_sha(40, '0');
    return resetBranch(branch, revnum, 0, null_sha, "delete");
}

int FastImportRepository::resetBranch(const QString &branch, int revnum, int mark, const QByteArray &resetTo, const QByteArray &comment)
{
    QByteArray branchRef = branch.toUtf8();
    if (!branchRef.startsWith("refs/"))
        branchRef.prepend("refs/heads/");

    Branch &br = branches[branch];
    if (br.created && br.created != revnum && br.marks.last()) {
        QByteArray backupBranch = "refs/backups/r" + QByteArray::number(revnum) + branchRef.mid(4);
        qWarning() << "backing up branch" << branch << "to" << backupBranch;

        fastImport.write("reset " + backupBranch + "\nfrom " + branchRef + "\n\n");
    }

    br.created = revnum;
    br.commits.append(revnum);
    br.marks.append(mark);

    fastImport.write("reset " + branchRef + "\nfrom " + resetTo + "\n\n"
                     "progress SVN r" + QByteArray::number(revnum)
                     + " branch " + branch.toUtf8() + " = :" + QByteArray::number(mark)
                     + " # " + comment + "\n\n");

    return EXIT_SUCCESS;
}

Repository::Transaction *FastImportRepository::newTransaction(const QString &branch, const QString &svnprefix,
                                                    int revnum)
{
    startFastImport();
    if (!branches.contains(branch)) {
        qWarning() << branch << "is not a known branch in repository" << name << endl
                   << "Going to create it automatically";
    }

    Transaction *txn = new Transaction;
    txn->repository = this;
    txn->branch = branch.toUtf8();
    txn->svnprefix = svnprefix.toUtf8();
    txn->datetime = 0;
    txn->revnum = revnum;

    if ((++commitCount % CommandLineParser::instance()->optionArgument(QLatin1String("commit-interval"), QLatin1String("10000")).toInt()) == 0) {
        // write everything to disk every 10000 commits
        fastImport.write("checkpoint\n");
        qDebug() << "checkpoint!, marks file trunkated";
    }
    outstandingTransactions++;
    return txn;
}

void FastImportRepository::forgetTransaction(Transaction *)
{
    if (!--outstandingTransactions)
        next_file_mark = maxMark;
}

void FastImportRepository::createAnnotatedTag(const QString &ref, const QString &svnprefix,
                                    int revnum,
                                    const QByteArray &author, uint dt,
                                    const QByteArray &log)
{
    QString tagName = ref;
    if (tagName.startsWith("refs/tags/"))
        tagName.remove(0, 10);

    if (!annotatedTags.contains(tagName))
        printf("Creating annotated tag %s (%s)\n", qPrintable(tagName), qPrintable(ref));
    else
        printf("Re-creating annotated tag %s\n", qPrintable(tagName));

    AnnotatedTag &tag = annotatedTags[tagName];
    tag.supportingRef = ref;
    tag.svnprefix = svnprefix.toUtf8();
    tag.revnum = revnum;
    tag.author = author;
    tag.log = log;
    tag.dt = dt;
}

void FastImportRepository::finalizeTags()
{
    if (annotatedTags.isEmpty())
        return;

    printf("Finalising tags for %s...", qPrintable(name));
    startFastImport();

    QHash<QString, AnnotatedTag>::ConstIterator it = annotatedTags.constBegin();
    for ( ; it != annotatedTags.constEnd(); ++it) {
        const QString &tagName = it.key();
        const AnnotatedTag &tag = it.value();

        QByteArray message = tag.log;
        if (!message.endsWith('\n'))
            message += '\n';
        if (CommandLineParser::instance()->contains("add-metadata"))
            message += "\nsvn path=" + tag.svnprefix + "; revision=" + QByteArray::number(tag.revnum) + "\n";

        {
            QByteArray branchRef = tag.supportingRef.toUtf8();
            if (!branchRef.startsWith("refs/"))
                branchRef.prepend("refs/heads/");

            QTextStream s(&fastImport);
            s << "progress Creating annotated tag " << tagName << " from ref " << branchRef << endl
              << "tag " << tagName << endl
              << "from " << branchRef << endl
              << "tagger " << QString::fromUtf8(tag.author) << ' ' << tag.dt << " -0000" << endl
              << "data " << message.length() << endl;
        }

        fastImport.write(message);
        fastImport.putChar('\n');
        if (!fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));

        printf(" %s", qPrintable(tagName));
        fflush(stdout);
    }

    while (fastImport.bytesToWrite())
        if (!fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
    printf("\n");
}

void FastImportRepository::startFastImport()
{
    if (fastImport.state() == QProcess::NotRunning) {
        if (processHasStarted)
            qFatal("git-fast-import has been started once and crashed?");
        processHasStarted = true;

        // start the process
        QString marksFile = marksFileName(name);
        QStringList marksOptions;
        marksOptions << "--import-marks=" + marksFile;
        marksOptions << "--export-marks=" + marksFile;
        marksOptions << "--force";

        fastImport.setStandardOutputFile(logFileName(name), QIODevice::Append);
        fastImport.setProcessChannelMode(QProcess::MergedChannels);

        if (!CommandLineParser::instance()->contains("dry-run")) {
            fastImport.start("git", QStringList() << "fast-import" << marksOptions);
        } else {
            fastImport.start("/bin/cat", QStringList());
        }

        reloadBranches();
    }
}

FastImportRepository::Transaction::~Transaction()
{
    repository->forgetTransaction(this);
}

void FastImportRepository::Transaction::setAuthor(const QByteArray &a)
{
    author = a;
}

void FastImportRepository::Transaction::setDateTime(uint dt)
{
    datetime = dt;
}

void FastImportRepository::Transaction::setLog(const QByteArray &l)
{
    log = l;
}

void FastImportRepository::Transaction::noteCopyFromBranch(const QString &branchFrom, int branchRevNum)
{
    if(branch == branchFrom) {
        qWarning() << "Cannot merge inside a branch";
        return;
    }
    static QByteArray dummy;
    int mark = repository->markFrom(branchFrom, branchRevNum, dummy);
    Q_ASSERT(dummy.isEmpty());

    if (mark == -1) {
        qWarning() << branch << "is copying from branch" << branchFrom
                    << "but the latter doesn't exist.  Continuing, assuming the files exist.";
    } else if (mark == 0) {
    qWarning() << "Unknown revision r" << QByteArray::number(branchRevNum)
               << ".  Continuing, assuming the files exist.";
    } else {
        qWarning() << "repository " + repository->name + " branch " + branch + " has some files copied from " + branchFrom + "@" + QByteArray::number(branchRevNum);

        if (!merges.contains(mark)) {
            merges.append(mark);
            qDebug() << "adding" << branchFrom + "@" + QByteArray::number(branchRevNum) << ":" << mark << "as a merge point";
        } else {
            qDebug() << "merge point already recorded";
        }
    }
}

void FastImportRepository::Transaction::deleteFile(const QString &path)
{
    QString pathNoSlash = path;
    if(pathNoSlash.endsWith('/'))
        pathNoSlash.chop(1);
    deletedFiles.append(pathNoSlash);
}

QIODevice *FastImportRepository::Transaction::addFile(const QString &path, int mode, qint64 length)
{
    int mark = repository->next_file_mark--;

    // in case the two mark allocations meet, we might as well just abort
    Q_ASSERT(mark > repository->last_commit_mark + 1);

    if (modifiedFiles.capacity() == 0)
        modifiedFiles.reserve(2048);
    modifiedFiles.append("M ");
    modifiedFiles.append(QByteArray::number(mode, 8));
    modifiedFiles.append(" :");
    modifiedFiles.append(QByteArray::number(mark));
    modifiedFiles.append(' ');
    modifiedFiles.append(path.toUtf8());
    modifiedFiles.append("\n");

    if (!CommandLineParser::instance()->contains("dry-run")) {
        repository->fastImport.write("blob\nmark :");
        repository->fastImport.write(QByteArray::number(mark));
        repository->fastImport.write("\ndata ");
        repository->fastImport.write(QByteArray::number(length));
        repository->fastImport.write("\n", 1);
    }

    return &repository->fastImport;
}

void FastImportRepository::Transaction::commit()
{
    processCache.touch(repository);

    // We might be tempted to use the SVN revision number as the fast-import commit mark.
    // However, a single SVN revision can modify multple branches, and thus lead to multiple
    // commits in the same repo.  So, we need to maintain a separate commit mark counter.
    int mark = ++repository->last_commit_mark;

    // in case the two mark allocations meet, we might as well just abort
    Q_ASSERT(mark < repository->next_file_mark - 1);

    // create the commit message
    QByteArray message = log;
    if (!message.endsWith('\n'))
        message += '\n';
    if (CommandLineParser::instance()->contains("add-metadata"))
        message += "\nsvn path=" + svnprefix + "; revision=" + QByteArray::number(revnum) + "\n";

    int parentmark = 0;
    Branch &br = repository->branches[branch];
    if (br.created && !br.marks.isEmpty()) {
        parentmark = br.marks.last();
    } else {
        qWarning() << "Branch" << branch << "in repository" << repository->name << "doesn't exist at revision"
                   << revnum << "-- did you resume from the wrong revision?";
        br.created = revnum;
    }
    br.commits.append(revnum);
    br.marks.append(mark);

    {
        QByteArray branchRef = branch;
        if (!branchRef.startsWith("refs/"))
            branchRef.prepend("refs/heads/");

        QTextStream s(&repository->fastImport);
        s << "commit " << branchRef << endl;
        s << "mark :" << QByteArray::number(mark) << endl;
        s << "committer " << QString::fromUtf8(author) << ' ' << datetime << " -0000" << endl;

        s << "data " << message.length() << endl;
    }

    repository->fastImport.write(message);
    repository->fastImport.putChar('\n');

    // note some of the inferred merges
    QByteArray desc = "";
    int i = !!parentmark;	// if parentmark != 0, there's at least one parent
    foreach (int merge, merges) {
        if (merge == parentmark) {
            qDebug() << "Skipping marking" << merge << "as a merge point as it matches the parent";
            continue;
        }

        if (++i > 16) {
            // FIXME: options:
            //   (1) ignore the 16 parent limit
            //   (2) don't emit more than 16 parents
            //   (3) create another commit on branch to soak up additional parents
            // we've chosen option (2) for now, since only artificial commits
            // created by cvs2svn seem to have this issue
            qWarning() << "too many merge parents";
           break;
        }

        QByteArray m = " :" + QByteArray::number(merge);
        desc += m;
        repository->fastImport.write("merge" + m + "\n");
    }

    // write the file deletions
    if (deletedFiles.contains(""))
        repository->fastImport.write("deleteall\n");
    else
        foreach (QString df, deletedFiles)
            repository->fastImport.write("D " + df.toUtf8() + "\n");

    // write the file modifications
    repository->fastImport.write(modifiedFiles);

    repository->fastImport.write("\nprogress SVN r" + QByteArray::number(revnum)
                                 + " branch " + branch + " = :" + QByteArray::number(mark)
                                 + (desc.isEmpty() ? "" : " # merge from") + desc
                                 + "\n\n");
    printf(" %d modifications from SVN %s to %s/%s",
           deletedFiles.count() + modifiedFiles.count(), svnprefix.data(),
           qPrintable(repository->name), branch.data());

    while (repository->fastImport.bytesToWrite())
        if (!repository->fastImport.waitForBytesWritten(-1))
            qFatal("Failed to write to process: %s", qPrintable(repository->fastImport.errorString()));
}
