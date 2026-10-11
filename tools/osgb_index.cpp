#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <osg/NodeVisitor>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/Registry>
#include <iostream>
#include <map>
#include <set>
#include <functional>

static QString canonical(const QString& path) {
    const QFileInfo info(path);
    return QDir::fromNativeSeparators(info.exists() ? info.canonicalFilePath() : info.absoluteFilePath());
}

struct References : osg::NodeVisitor {
    QString directory;
    std::set<QString> files;
    int paged = 0, overlapping = 0;
    explicit References(const QString& dir) : NodeVisitor(TRAVERSE_ALL_CHILDREN), directory(dir) {
        setNodeMaskOverride(~0u);
    }
    template<class T> void collect(T& node) {
        QDir base(directory);
        if(!node.getDatabasePath().empty()) base=QDir(base.filePath(QString::fromStdString(node.getDatabasePath())));
        for(unsigned i=0;i<node.getNumFileNames();++i) {
            const auto name=QString::fromStdString(node.getFileName(i));
            if(!name.isEmpty()) files.insert(canonical(base.filePath(name)));
        }
    }
    void apply(osg::PagedLOD& node) override {
        ++paged; collect(node);
        // Report overlapping authored LOD ranges without changing their semantics.
        for(unsigned i=0;i<node.getNumRanges();++i) for(unsigned j=i+1;j<node.getNumRanges();++j)
            if(std::max(node.getMinRange(i),node.getMinRange(j)) < std::min(node.getMaxRange(i),node.getMaxRange(j))) ++overlapping;
        traverse(node);
    }
    void apply(osg::ProxyNode& node) override { collect(node); traverse(node); }
};

struct PagePaths : osg::NodeVisitor {
    QString directory, entryDirectory;
    PagePaths(const QString& dir, const QString& entryDir) : NodeVisitor(TRAVERSE_ALL_CHILDREN), directory(dir), entryDirectory(entryDir) { setNodeMaskOverride(~0u); }
    template<class T> void fix(T& node) {
        const QString path=node.getDatabasePath().empty()?directory:QDir(directory).filePath(QString::fromStdString(node.getDatabasePath()));
        // Resolve original page references, then store them relative to the entry.
        const QDir base(path);
        for(unsigned i=0;i<node.getNumFileNames();++i) {
            const QString name=QString::fromStdString(node.getFileName(i));
            if(!name.isEmpty()) node.setFileName(i,QDir(entryDirectory).relativeFilePath(base.filePath(name)).toUtf8().toStdString());
        }
        node.setDatabasePath("");
    }
    void apply(osg::PagedLOD& node) override { fix(node); traverse(node); }
    void apply(osg::ProxyNode& node) override { fix(node); traverse(node); }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    const bool sameDirectory=args.size()==4 && args[3]=="--same-directory";
    if(args.size()!=3 && !sameDirectory) { std::cerr<<"Usage: gl3d_osgb_index INPUT_DIRECTORY OUTPUT.osgb [--same-directory]\n"; return 2; }
    const QString source=canonical(args[1]), output=QFileInfo(args[2]).absoluteFilePath();
    if(!QDir(source).exists() || QFileInfo(output).exists()) {
        std::cerr<<"Input directory missing or output already exists\n"; return 2;
    }
    osgDB::Registry::instance()->getLibraryFilePathList().push_back((app.applicationDirPath()+"/osgPlugins-3.6.5").toStdString());
    std::map<QString,std::set<QString>> graph;
    QDirIterator it(source,{"*.osgb","*.OSGB"},QDir::Files,QDirIterator::Subdirectories);
    while(it.hasNext()) graph.emplace(canonical(it.next()),std::set<QString>());
    if(graph.empty()) { std::cerr<<"No OSGB files\n"; return 2; }
    std::set<QString> incoming, missing, empty;
    int scanned=0, paged=0, overlapping=0;
    osg::ref_ptr<osgDB::Options> options=new osgDB::Options;
    options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    for(auto& entry:graph) {
        auto node=osgDB::readRefNodeFile(entry.first.toUtf8().toStdString(),options);
        if(!node) { std::cerr<<"Unreadable file: "<<entry.first.toUtf8().constData()<<std::endl; return 1; }
        References refs(QFileInfo(entry.first).absolutePath()); node->accept(refs);
        if (!node->getBound().valid() && refs.files.empty()) empty.insert(entry.first);
        entry.second=std::move(refs.files); paged+=refs.paged; overlapping+=refs.overlapping;
        for(const auto& ref:entry.second) {
            if(graph.count(ref)) incoming.insert(ref);
            else if(!QFileInfo::exists(ref)) missing.insert(ref);
        }
        if(++scanned%100==0 || scanned==int(graph.size())) std::cout<<"Scanned "<<scanned<<"/"<<graph.size()<<std::endl;
    }
    QStringList roots;
    for(const auto& entry:graph) if(!incoming.count(entry.first) && !empty.count(entry.first)) roots.append(entry.first);
    std::set<QString> visited, active;
    bool cycle=false;
    std::function<void(const QString&)> visit=[&](const QString& file) {
        if(active.count(file)) { cycle=true; return; }
        if(visited.count(file) || !graph.count(file)) return;
        visited.insert(file); active.insert(file);
        for(const auto& child:graph.at(file)) visit(child);
        active.erase(file);
    };
    for(const auto& root:roots) visit(root);
    bool unreachable=false;
    for (const auto& entry:graph) if(!visited.count(entry.first) && !empty.count(entry.first)) unreachable=true;
    if(roots.isEmpty() || cycle || unreachable) { std::cerr<<"Reference cycle or unreachable files; no index written\n"; return 1; }
    osg::ref_ptr<osg::Group> group=new osg::Group;
    osg::ref_ptr<osg::Node> scene;
    for(const auto& root:roots) {
        auto node=osgDB::readRefNodeFile(root.toUtf8().toStdString(),options);
        if(!node) return 1;
        PagePaths paths(QFileInfo(root).absolutePath(),sameDirectory?source:QFileInfo(output).absolutePath()); node->accept(paths);
        group->addChild(node); scene=node;
        std::cout<<"Root: "<<root.toUtf8().constData()<<std::endl;
    }
    if(roots.size()>1) scene=group;
    QDir().mkpath(QFileInfo(output).absolutePath());
    osg::ref_ptr<osgDB::Options> writeOptions=new osgDB::Options("WriteImageHint=IncludeData Compressor=zlib");
    if(!osgDB::writeNodeFile(*scene,output.toUtf8().toStdString(),writeOptions)) { std::cerr<<"Index write failed\n"; return 1; }
    QJsonArray rootNames, missingNames, emptyNames;
    for(const auto& root:roots) rootNames.append(root);
    for(const auto& file:missing) missingNames.append(file);
    for(const auto& file:empty) emptyNames.append(file);
    QJsonObject report{{"source",source},{"output",output},{"files",int(graph.size())},{"roots",rootNames},
        {"emptyFiles",emptyNames},{"reachableFiles",int(visited.size())},{"pagedLodNodes",paged},{"overlappingRangePairs",overlapping},{"missingPages",missingNames}};
    QSaveFile json(output+".json");
    if(!json.open(QIODevice::WriteOnly)) return 1;
    json.write(QJsonDocument(report).toJson()); if(!json.commit()) return 1;
    std::cout<<"Written "<<output.toUtf8().constData()<<"; roots="<<roots.size()<<"; missing="<<missing.size()<<"; overlapping="<<overlapping<<std::endl;
    return 0;
}
