#include "lstmsdparser/lstm_sdparser.h"

namespace ltp {
namespace lstmsdparser {

using namespace dynet::expr;
using namespace dynet;
using namespace std;
namespace po = boost::program_options;

std::string StrToLower(const std::string s){
  std::string str = s;
  for (int i = 0; i < str.length(); ++i){
    str[i] = tolower(str[i]);
  }
  return str;
}

//struct LSTMParser {

LSTMParser::LSTMParser(): Opt({2, 100, 200, 50, 100, 200, 50, 50, 100, 0, "list-tree", 
                                "", "4000", true, false, false, false}) {}

LSTMParser::~LSTMParser() {}

void LSTMParser::set_options(Options opts){
  this->Opt = opts;
}

bool LSTMParser::load(string model_file, string training_data_file, string word_embedding_file,
                        string dev_data_file){
  this->transition_system = Opt.transition_system;
  corpus.set_transition_system(Opt.transition_system);
  if (DEBUG)
    cerr << "Loading training data from " << training_data_file << endl;
  //corpus.load_correct_actions(training_data_file);
  corpus.load_conll_file(training_data_file);
  if (DEBUG)
    cerr << "finish loading training data" << endl;

  kUNK = corpus.get_or_add_word(cpyp::Corpus::UNK);

  if (word_embedding_file.length() > 0){
    pretrained[kUNK] = std::vector<float>(Opt.PRETRAINED_DIM, 0);
    if (DEBUG)
      cerr << "Loading word embeddings from " << word_embedding_file << " with " << Opt.PRETRAINED_DIM << " dimensions\n";
    ifstream in(word_embedding_file.c_str());
    string line;
    getline(in, line);
    std::vector<float> v(Opt.PRETRAINED_DIM, 0);
    string word;
    while (getline(in, line)) {
      istringstream lin(line);
      lin >> word;
      for (unsigned i = 0; i < Opt.PRETRAINED_DIM; ++i) lin >> v[i];
      unsigned id = corpus.get_or_add_word(word);
      pretrained[id] = v;
    }
  }

  get_dynamic_infos();
  if (DEBUG)
    cerr << "Setup model in dynet" << endl;
  //allocate memory for dynet
  char ** dy_argv = new char * [6];
  int dy_argc = 3;
  dy_argv[0] = "dynet";
  dy_argv[1] = "--dynet-mem";
  dy_argv[2] = (char*)Opt.dynet_mem.c_str();
  if (Opt.dynet_seed.length() > 0){
    dy_argc = 5;
    dy_argv[3] = "--dynet-seed";
    dy_argv[4] = (char*)Opt.dynet_seed.c_str();
  }
  //argv[3] = nullptr;
  //auto dyparams = dynet::extract_dynet_params(argc, argv);
  dynet::initialize(dy_argc, dy_argv);
  delete dy_argv;
  //dynet::initialize(dyparams);

  stack_lstm = LSTMBuilder(Opt.LAYERS, Opt.LSTM_INPUT_DIM, Opt.HIDDEN_DIM, model);
  buffer_lstm = LSTMBuilder(Opt.LAYERS, Opt.LSTM_INPUT_DIM, Opt.HIDDEN_DIM, model);
  pass_lstm = LSTMBuilder(Opt.LAYERS, Opt.LSTM_INPUT_DIM, Opt.HIDDEN_DIM, model);
  action_lstm = LSTMBuilder(Opt.LAYERS, Opt.ACTION_DIM, Opt.HIDDEN_DIM, model);
  p_w = model.add_lookup_parameters(System_size.VOCAB_SIZE, {Opt.INPUT_DIM});
  p_a = model.add_lookup_parameters(System_size.ACTION_SIZE, {Opt.ACTION_DIM});
  p_r = model.add_lookup_parameters(System_size.ACTION_SIZE, {Opt.REL_DIM});
  p_pbias = model.add_parameters({Opt.HIDDEN_DIM});
  p_A = model.add_parameters({Opt.HIDDEN_DIM, Opt.HIDDEN_DIM});
  p_B = model.add_parameters({Opt.HIDDEN_DIM, Opt.HIDDEN_DIM});
  p_P = model.add_parameters({Opt.HIDDEN_DIM, Opt.HIDDEN_DIM});
  p_S = model.add_parameters({Opt.HIDDEN_DIM, Opt.HIDDEN_DIM});
  p_H = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.LSTM_INPUT_DIM});
  p_D = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.LSTM_INPUT_DIM});
  p_R = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.REL_DIM});
  p_w2l = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.INPUT_DIM});
  p_ib = model.add_parameters({Opt.LSTM_INPUT_DIM});
  p_cbias = model.add_parameters({Opt.LSTM_INPUT_DIM});
  p_p2a = model.add_parameters({System_size.ACTION_SIZE, Opt.HIDDEN_DIM});
  p_action_start = model.add_parameters({Opt.ACTION_DIM});
  p_abias = model.add_parameters({System_size.ACTION_SIZE});
  p_buffer_guard = model.add_parameters({Opt.LSTM_INPUT_DIM});
  p_stack_guard = model.add_parameters({Opt.LSTM_INPUT_DIM});
  p_pass_guard = model.add_parameters({Opt.LSTM_INPUT_DIM});
  if (Opt.USE_BILSTM) {
    buffer_bilstm = BidirectionalLSTMLayer(model, Opt.LAYERS, Opt.LSTM_INPUT_DIM, Opt.BILSTM_HIDDEN_DIM);
    p_fwB = model.add_parameters({Opt.HIDDEN_DIM, Opt.BILSTM_HIDDEN_DIM});
    p_bwB = model.add_parameters({Opt.HIDDEN_DIM, Opt.BILSTM_HIDDEN_DIM});
    cerr << "Created Buffer BiLSTM" << endl;
  }
  if (Opt.USE_TREELSTM) {
    tree_lstm = TheirTreeLSTMBuilder(1, Opt.LSTM_INPUT_DIM, Opt.LSTM_INPUT_DIM, model);
    cerr << "Created TreeLSTM" << endl;
  }
  if (Opt.USE_POS) {
    p_p = model.add_lookup_parameters(System_size.POS_SIZE, {Opt.POS_DIM});
    p_p2l = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.POS_DIM});
  }
  if (pretrained.size() > 0) {
    use_pretrained = true;
    p_t = model.add_lookup_parameters(System_size.VOCAB_SIZE, {Opt.PRETRAINED_DIM});
    for (auto it : pretrained)
      p_t.initialize(it.first, it.second);
    p_t2l = model.add_parameters({Opt.LSTM_INPUT_DIM, Opt.PRETRAINED_DIM});
  }else {
    use_pretrained = false;
    //p_t = nullptr;
    //p_t2l = nullptr;
  }

  //this->model = model;
  if (model_file.length() > 0) {
    if (DEBUG)
      cerr << "loading model from " << model_file << endl;
    ifstream in(model_file.c_str());
    boost::archive::text_iarchive ia(in);
    ia >> this->model;
    if (DEBUG)
      cerr << "finish loading model" << endl;
  }
  if (dev_data_file.length() > 0){
    if (DEBUG)
      cerr << "loading dev data from " << dev_data_file << endl;
    //corpus.load_correct_actionsDev(dev_data_file);
    corpus.load_conll_fileDev(dev_data_file);
    if (DEBUG)
      cerr << "finish loading dev data" << endl;
  }
  return true;
}

void LSTMParser::get_dynamic_infos(){
  
  System_size.kROOT_SYMBOL = corpus.get_or_add_word(ltp::lstmsdparser::ROOT_SYMBOL);

  {  // compute the singletons in the parser's training data
    map<unsigned, unsigned> counts;
    for (auto sent : corpus.sentences)
      for (auto word : sent.second) { training_vocab.insert(word); counts[word]++; }
    for (auto wc : counts)
      if (wc.second == 1) singletons.insert(wc.first);
  }
  if (DEBUG)
    cerr << "Number of words: " << corpus.nwords << endl;
  System_size.VOCAB_SIZE = corpus.nwords + 1;
  //ACTION_SIZE = corpus.nactions + 1;
  System_size.ACTION_SIZE = corpus.nactions + 30; // leave places for new actions in test set
  System_size.POS_SIZE = corpus.npos + 10;  // bad way of dealing with the fact that we may see new POS tags in the test set
  possible_actions.resize(corpus.nactions);
  for (unsigned i = 0; i < corpus.nactions; ++i)
    possible_actions[i] = i;
}
/*
bool LSTMParser::has_path_to(int w1, int w2, const vector<bool>  dir_graph []){
    //cerr << endl << w1 << " has path to " << w2 << endl;
    if (dir_graph[w1][w2])
        return true;
    for (int i = 0; i < (int)dir_graph[w1].size(); ++i){
        if (dir_graph[w1][i])
            if (has_path_to(i, w2, dir_graph))
                return true;
    }
    return false;
}*/

bool LSTMParser::has_path_to(int w1, int w2, const vector<vector<string>>& graph){
    //cerr << endl << w1 << " has path to " << w2 << endl;
    if (graph[w1][w2] != REL_NULL)
        return true;
    for (int i = 0; i < (int)graph.size(); ++i){
        if (graph[w1][i] != REL_NULL)
            if (has_path_to(i, w2, graph))
                return true;
    }
    return false;
}

bool LSTMParser::has_path_to(int w1, int w2, const vector<vector<bool>>& graph){
    //cerr << endl << w1 << " has path to " << w2 << endl;
    if (graph[w1][w2])
        return true;
    for (int i = 0; i < (int)graph.size(); ++i){
        if (graph[w1][i])
            if (has_path_to(i, w2, graph))
                return true;
    }
    return false;
}

vector<unsigned> LSTMParser::get_children(unsigned id, const vector<vector<bool>> graph){
  vector<unsigned> children;
  for (int i = 0; i < unsigned(graph[0].size()); i++){
    if (graph[id][i])
      children.push_back(i);
  }
  return children;
}

bool LSTMParser::IsActionForbidden(const string& a, unsigned bsize, unsigned ssize, 
                                      unsigned root, const vector<vector<bool>> dir_graph, //const vector<bool>  dir_graph [], 
                                      const vector<int>& stacki, const vector<int>& bufferi) {
    if (transition_system == "list-graph"){
        //cerr << a << endl;
        int s0 = stacki.back();
        int b0 = bufferi.back();
        int root_num = 0;
        int s0_head_num = 0;
        for (int i = 0; i < (int)dir_graph[root].size(); ++i)
            if (dir_graph[root][i])
                root_num ++;
        if (s0 >= 0)
            for (int i = 0; i < (int)dir_graph[root].size(); ++i)
                if (dir_graph[i][s0])
                    s0_head_num ++;
        if (a[0] == 'L'){
            string rel = a.substr(3, a.size() - 4);
            if (bsize < 2 || ssize < 2) return true;
            if (has_path_to(s0, b0, dir_graph)) return true;
            //if (b0 == root && rel != "Root") return true;
            //if (b0 == root && rel == "Root" && root_num >= 1) return true;
            if (b0 == (int)root && !(StrToLower(rel) == "root" && root_num == 0 && s0_head_num == 0)) return true;
            if (b0 != (int)root && StrToLower(rel) == "root") return true;
        }
        if (a[0] == 'R'){
            if (bsize < 2 || ssize < 2) return true;
            if (has_path_to(b0, s0, dir_graph)) return true;
            if (b0 == (int)root) return true;
        }
        if (a[0] == 'N'){
            if (a[1] == 'S' && bsize < 2) return true;
            //if (a[1] == 'S' && bsize == 2 && ssize > 2) return true;
            if (a[1] == 'R' && !(ssize > 1 && s0_head_num > 0)) return true;
            if (a[1] == 'P' && !(ssize > 1 && bsize > 1))  return true;
        }
        return false;
  }
    else if (transition_system == "list-tree"){
        int s0 = stacki.back();
        int b0 = bufferi.back();
        int root_num = 0;
        int s0_head_num = 0;
        int b0_head_num = 0;
        for (int i = 0; i < (int)dir_graph[root].size(); ++i)
            if (dir_graph[root][i])
                root_num ++;
        if (s0 >= 0)
            for (int i = 0; i < (int)dir_graph[root].size(); ++i)
                if (dir_graph[i][s0])
                    s0_head_num ++;
        if (b0 >= 0)
            for (int i = 0; i < (int)dir_graph[root].size(); ++i)
                if (dir_graph[i][b0])
                    b0_head_num ++;
        if (a[0] == 'L'){
            string rel = a.substr(3, a.size() - 4);
            if (bsize < 2 || ssize < 2) return true;
            if (has_path_to(s0, b0, dir_graph)) return true;
            //if (b0 == root && rel != "Root") return true;
            //if (b0 == root && rel == "Root" && root_num >= 1) return true;
            if (b0 == (int)root && !(StrToLower(rel) == "root" && root_num == 0 && s0_head_num == 0)) return true;
            if (b0 != (int)root && StrToLower(rel) == "root") return true;
            if (s0_head_num >= 1) return true; // add for original list-based
        }
        if (a[0] == 'R'){
            if (bsize < 2 || ssize < 2) return true;
            if (has_path_to(b0, s0, dir_graph)) return true;
            if (b0 == (int)root) return true;
            if (b0_head_num >= 1) return true; // add for original list-based
        }
        if (a[0] == 'N'){
            if (a[1] == 'S' && bsize < 2) return true;
            //if (a[1] == 'S' && bsize == 2 && ssize > 2) return true;
            if (a[1] == 'R' && !(ssize > 1 && s0_head_num > 0)) return true;
            if (a[1] == 'P' && !(ssize > 1 && bsize > 1))  return true;
        }
        return false;
  }
    return false;
}

bool LSTMParser::IsActionForbidden2(const string& a, unsigned bsize, unsigned ssize, vector<int> stacki) 
{ 
    if (a[1]=='W' && ssize<=3) return true; //MIGUEL

    if (a[1]=='W') { //MIGUEL
        int top=stacki[stacki.size()-1];
        int sec=stacki[stacki.size()-2];
        if (sec>top) return true;
    }

    bool is_shift = (a[0] == 'S' && a[1]=='H');  //MIGUEL
    bool is_reduce = !is_shift;
    if (is_shift && bsize == 1) return true;
    if (is_reduce && ssize < 3) return true;
    if (bsize == 2 && // ROOT is the only thing remaining on buffer
        ssize > 2 && // there is more than a single element on the stack
        is_shift) return true;
    // only attach left to ROOT
    //if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
    if (a[0] == 'L' || a[0] == 'R'){
        string rel = (a[0] == 'L') ? a.substr(9, a.size() - 10) : a.substr(10, a.size() - 11);
        rel = StrToLower(rel);
	if (bsize == 1 && ssize == 3 && a[0] == 'R' && rel != "root") return true;
	if (bsize == 1 && ssize == 3 && a[0] == 'L' && rel != "root") return true; 
    }
    return false;
}

vector<vector<string>> LSTMParser::compute_heads(const vector<unsigned>& sent, const vector<unsigned>& actions) {
  //map<int,int> heads;
  //map<int,string> r;
  //map<int,string>& rels = (pr ? *pr : r);
    const vector<string>& setOfActions = corpus.actions;
    unsigned sent_len = sent.size();
    vector<vector<string>> graph;
     
    for (unsigned i = 0; i < sent_len; i++) {
        vector<string> r;
        for (unsigned j = 0; j < sent_len; j++) r.push_back(REL_NULL);
        graph.push_back(r);
    }
    vector<int> bufferi(sent_len + 1, 0), stacki(1, -999), passi(1, -999);
    for (unsigned i = 0; i < sent_len; ++i)
        bufferi[sent_len - i] = i;
    bufferi[0] = -999;
    for (auto action: actions) { // loop over transitions for sentence
        const string& actionString=setOfActions[action];
        const char ac = actionString[0];
        const char ac2 = actionString[1];

      /*cerr <<endl<<"[";
      for (int i = (int)stacki.size() - 1; i > -1 ; --i)
        cerr << corpus.intToWords[sent[stacki[i]]] <<"-"<<stacki[i]<<", ";
      cerr <<"][";
      for (int i = (int)passi.size() - 1; i > -1 ; --i)
        cerr << corpus.intToWords[sent[passi[i]]]<<"-"<<passi[i]<<", ";
      cerr <<"][";
      for (int i = (int)bufferi.size() - 1; i > -1 ; --i)
        cerr << corpus.intToWords[sent[bufferi[i]]]<<"-"<<bufferi[i]<<", ";
      cerr <<"]"<<endl;

      cerr << "action:" << actionString << endl;*/

        if (transition_system == "list-graph" || transition_system == "list-tree"){
            if (ac =='N' && ac2=='S') {  // NO-SHIFT
                assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
                int passi_size = (int)passi.size();
                for (int i = 1; i < passi_size; i++){  //do not move pass_guard
                    stacki.push_back(passi.back());
                    passi.pop_back();
                }
                stacki.push_back(bufferi.back());
                bufferi.pop_back();
            } else if (ac=='N' && ac2=='R'){
                assert(stacki.size() > 1);
                stacki.pop_back();
            } else if (ac=='N' && ac2=='P'){
                assert(stacki.size() > 1);
                passi.push_back(stacki.back());
                stacki.pop_back();
            } else if (ac=='L'){ // LEFT-REDUCE or LEFT-PASS
                assert(stacki.size() > 1 && bufferi.size() > 1);
                unsigned depi, headi;
                depi = stacki.back();
                stacki.pop_back();
                headi = bufferi.back();
                graph[headi][depi] = actionString.substr(3, actionString.size() - 4);
                if (ac2 == 'P'){ // LEFT-PASS
                    //TODO pass_lstm
                    passi.push_back(depi);
                }
            } else if (ac=='R'){ // RIGHT-SHIFT or RIGHT-PASSA
                assert(stacki.size() > 1 && bufferi.size() > 1);
                unsigned depi, headi;
                depi = bufferi.back();
                bufferi.pop_back();
                headi = stacki.back();
                stacki.pop_back();
                graph[headi][depi] = actionString.substr(3, actionString.size() - 4);
                if (ac2 == 'S'){ //RIGHT-SHIFT
                    stacki.push_back(headi);
                    int passi_size = (int)passi.size();
                    for (int i = 1; i < passi_size; i++){  //do not move pass_guard
                        stacki.push_back(passi.back());
                        passi.pop_back();
                    }
                    stacki.push_back(depi);
                }
                else if (ac2 == 'P'){
                    //TODO pass_lstm.add_input(nlcomposed);
                    passi.push_back(headi);
                    bufferi.push_back(depi);
                }
            }
        }
        else if (transition_system == "swap"){
            if (ac =='S' && ac2=='H') {  // SHIFT
                assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
                stacki.push_back(bufferi.back());
                bufferi.pop_back();
            } 
            else if (ac=='S' && ac2=='W') {
                assert(stacki.size() > 2);
                unsigned ii = 0, jj = 0;
                jj=stacki.back();
                stacki.pop_back();
                ii=stacki.back();
                stacki.pop_back();
                bufferi.push_back(ii);
                stacki.push_back(jj);
            }
            else { // LEFT or RIGHT
                assert(stacki.size() > 2); // dummy symbol means > 2 (not >= 2)
                assert(ac == 'L' || ac == 'R');
                unsigned depi = 0, headi = 0;
                (ac == 'R' ? depi : headi) = stacki.back();
                stacki.pop_back();
                (ac == 'R' ? headi : depi) = stacki.back();
                stacki.pop_back();
                stacki.push_back(headi);
                int offset = (ac == 'R' ? 10 : 9);
                graph[headi][depi] = actionString.substr(offset, actionString.size() - offset - 1);
                //cerr << graph[headi][depi] << endl;
            }
        }
    }
  assert(bufferi.size() == 1);
  //assert(stacki.size() == 2);
  return graph;
}

// *** if correct_actions is empty, this runs greedy decoding ***
// returns parse actions for input sentence (in training just returns the reference)
// OOV handling: raw_sent will have the actual words
//               sent will have words replaced by appropriate UNK tokens
// this lets us use pretrained embeddings, when available, for words that were OOV in the
// parser training data
vector<unsigned> LSTMParser::log_prob_parser(ComputationGraph* hg,
                     const vector<unsigned>& raw_sent,  // raw sentence
                     const vector<unsigned>& sent,  // sent with oovs replaced
                     const vector<unsigned>& sentPos,
                     const vector<unsigned>& correct_actions,
                     const vector<string>& setOfActions,
                     const map<unsigned, std::string>& intToWords,
                     double *right, 
                     vector<vector<string>>& cand,
                     vector<Expression>* word_rep,
                     Expression * act_rep) {
    //const vector<string> setOfActions = corpus.actions;
    //const map<unsigned, std::string> intToWords = corpus.intToWords;

    vector<unsigned> results;
    const bool build_training_graph = correct_actions.size() > 0;
    //init word representation
    if (word_rep){
        for (unsigned i = 0; i < sent.size(); ++i){
            Expression wd;
            (*word_rep).push_back(wd);
        }
    }
    //init candidate
    vector<string> cv;
    for (unsigned i = 0; i < sent.size(); ++i)
        cv.push_back(REL_NULL);
    for (unsigned i = 0; i < sent.size(); ++i)
        cand.push_back(cv);

    stack_lstm.new_graph(*hg);
    stack_lstm.start_new_sequence();
    if (transition_system == "list-graph" || transition_system == "list-tree"){
      pass_lstm.new_graph(*hg);
      pass_lstm.start_new_sequence();
    }
    action_lstm.new_graph(*hg);
    action_lstm.start_new_sequence();

    Expression fwB;
    Expression bwB;
    if (Opt.USE_BILSTM){
      buffer_bilstm.new_graph(hg); // [bilstm] start_new_sequence is implemented in add_input
      fwB = parameter(*hg, p_fwB); // [bilstm]
      bwB = parameter(*hg, p_bwB); // [bilstm]
    }else{
      buffer_lstm.new_graph(*hg);
      buffer_lstm.start_new_sequence();
    }
    if (Opt.USE_TREELSTM){
      tree_lstm.new_graph(*hg); // [treelstm]
      tree_lstm.start_new_sequence(); // [treelstm]
      tree_lstm.initialize_structure(sent.size()); // [treelstm]
    }

    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression H = parameter(*hg, p_H);
    Expression D = parameter(*hg, p_D);
    Expression R = parameter(*hg, p_R);
    Expression cbias = parameter(*hg, p_cbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression P = parameter(*hg, p_P);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (Opt.USE_POS)
      p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (use_pretrained)
      t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    action_lstm.add_input(action_start);
    vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
    // precompute buffer representation from left to right
    vector<Expression> word_emb(sent.size()); // [treelstm] store original word representation emb[i] for sent[i]

    for (unsigned i = 0; i < sent.size(); ++i) {
      assert(sent[i] < System_size.VOCAB_SIZE);
      Expression w =lookup(*hg, p_w, sent[i]);

      vector<Expression> args = {ib, w2l, w}; // learn embeddings
      if (Opt.USE_POS) { // learn POS tag?
        Expression p = lookup(*hg, p_p, sentPos[i]);
        args.push_back(p2l);
        args.push_back(p);
      }
      if (use_pretrained && pretrained.count(raw_sent[i])) {  // include fixed pretrained vectors?
        Expression t = const_lookup(*hg, p_t, raw_sent[i]);
        args.push_back(t2l);
        args.push_back(t);
      }
      //buffer[] = ib + w2l * w + p2l * p + t2l * t
      buffer[sent.size() - i] = rectify(affine_transform(args));
      bufferi[sent.size() - i] = i;
      if (Opt.USE_TREELSTM){
        word_emb[i] = buffer[sent.size() - i];
      }
    }
    if (Opt.USE_TREELSTM){
      vector<unsigned> h;
      for (int i = 0; i < sent.size(); i++)
        tree_lstm.add_input(i, h, word_emb[i]);
    }

    // dummy symbol to represent the empty buffer
    buffer[0] = parameter(*hg, p_buffer_guard);
    bufferi[0] = -999;
    std::vector<BidirectionalLSTMLayer::Output> bilstm_outputs;
    if (Opt.USE_BILSTM){
      buffer_bilstm.add_inputs(hg, buffer); 
      buffer_bilstm.get_outputs(hg, bilstm_outputs); // [bilstm] output of bilstm for buffer, first is fw, second is bw
    }else{
      for (auto& b : buffer)
        buffer_lstm.add_input(b);
    }

    vector<Expression> pass; //variables reperesenting embedding in pass buffer
    vector<int> passi; //position of words in pass buffer
    if (Opt.transition_system == "list-graph" || Opt.transition_system == "list-tree"){   
      pass.push_back(parameter(*hg, p_pass_guard));
      passi.push_back(-999); // not used for anything
      pass_lstm.add_input(pass.back());
    }

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki; // position of words in the sentence of head of subtree
    stack.push_back(parameter(*hg, p_stack_guard));
    stacki.push_back(-999); // not used for anything
    // drive dummy symbol on stack through LSTM
    stack_lstm.add_input(stack.back());
    vector<Expression> log_probs;
    string rootword;
    unsigned action_count = 0;  // incremented at each prediction

    //init graph connecting vector
    //vector<bool> dir_graph[sent.size()]; // store the connection between words in sent
    vector<vector<bool>> dir_graph;
    vector<bool> v;
    for (int i = 0; i < (int)sent.size(); i++){
        v.push_back(false);
    }
    for (int i = 0; i < (int)sent.size(); i++){
        dir_graph.push_back(v);
    }
    //remove stack.size() > 2 ||
    while(((transition_system == "list-graph" || transition_system == "list-tree") && bufferi.size() > 1)
          || (transition_system == "swap" && (stacki.size() > 2 || bufferi.size() > 1))) {
      // get list of possible actions for the current parser state
      vector<unsigned> current_valid_actions;
      /*if (!build_training_graph){
      cerr << sent.size() << endl;
      cerr <<endl<<"[";
      for (int i = (int)stacki.size() - 1; i > -1 ; --i)
        cerr << corpus.intToWords[sent[stacki[i]]] <<"-"<<stacki[i]<<", ";
      cerr <<"][";
      //for (int i = (int)passi.size() - 1; i > -1 ; --i)
      //  cerr << corpus.intToWords[sent[passi[i]]]<<"-"<<passi[i]<<", ";
      //cerr <<"][";
      for (int i = (int)bufferi.size() - 1; i > -1 ; --i)
        cerr << corpus.intToWords[sent[bufferi[i]]]<<"-"<<bufferi[i]<<", ";
      cerr <<"]"<<endl;
      //}*/
      if (transition_system == "list-graph" || transition_system == "list-tree")
        for (auto a: possible_actions) {
          //cerr << " " << setOfActions[a]<< " ";
          if (IsActionForbidden(setOfActions[a], buffer.size(), stack.size(), sent.size() - 1, dir_graph, stacki, bufferi))
            continue;
          //cerr << " <" << setOfActions[a] << "> ";
          current_valid_actions.push_back(a);
        }
      else if (transition_system == "swap")
        for (auto a: possible_actions) {
          if (IsActionForbidden2(setOfActions[a], bufferi.size(), stacki.size(), stacki))
            continue;
          //cerr << " <" << setOfActions[a] << "> ";
          current_valid_actions.push_back(a);
        }

      Expression p_t;
      if (Opt.USE_BILSTM){
        //cerr << "bilstm: " << bilstm_outputs.size() << " id: " 
        //<< sent.size() - bufferi.back() << " bufferi: " << bufferi.back() << endl;
        Expression fwbuf,bwbuf;
        int idx;
        if (bufferi.size() > 1)
          idx = sent.size() - bufferi.back();
        else
          idx = 0;
        fwbuf = bilstm_outputs[idx].first - bilstm_outputs[1].first;
        bwbuf = bilstm_outputs[1].second - bilstm_outputs[idx].second;
        // [bilstm] p_t = pbias + S * slstm + P * plstm + fwB * blstm_fw + bwB * blstm_bw + A * almst
        /*p_t = affine_transform({pbias, S, stack_lstm.back(), P, pass_lstm.back(), 
          fwB, bilstm_outputs[sent.size() - bufferi.back()].first, bwB, bilstm_outputs[sent.size() - bufferi.back()].second,
          A, action_lstm.back()});*/
        if (transition_system == "list-graph" || transition_system == "list-tree")
          p_t = affine_transform({pbias, S, stack_lstm.back(), P, pass_lstm.back(), 
                                fwB, fwbuf, bwB, bwbuf, A, action_lstm.back()});
        else if (transition_system == "swap")
          p_t = affine_transform({pbias, S, stack_lstm.back(), fwB, fwbuf, bwB, bwbuf, 
                                A, action_lstm.back()});
        //cerr << " bilstm: " << sent.size() - bufferi.back() << endl;
      }else{
        if (transition_system == "list-graph" || transition_system == "list-tree")
          // p_t = pbias + S * slstm + P * plstm + B * blstm + A * almst
          p_t = affine_transform({pbias, S, stack_lstm.back(), P, pass_lstm.back(), B, buffer_lstm.back(), A, action_lstm.back()});
        else if (transition_system == "swap")
          p_t = affine_transform({pbias, S, stack_lstm.back(), B, buffer_lstm.back(), A, action_lstm.back()});
      }
      Expression nlp_t = rectify(p_t);
      // r_t = abias + p2a * nlp
      Expression r_t = affine_transform({abias, p2a, nlp_t});

      // adist = log_softmax(r_t, current_valid_actions)
      Expression adiste = log_softmax(r_t, current_valid_actions);
      vector<float> adist = as_vector(hg->incremental_forward(adiste));
      double best_score = adist[current_valid_actions[0]];
      unsigned best_a = current_valid_actions[0];
      for (unsigned i = 1; i < current_valid_actions.size(); ++i) {
        if (adist[current_valid_actions[i]] > best_score) {
          best_score = adist[current_valid_actions[i]];
          best_a = current_valid_actions[i];
        }
      }
      unsigned action = best_a;
      if (build_training_graph) {  // if we have reference actions (for training) use the reference action
        action = correct_actions[action_count];
        if (best_a == action) { (*right)++; }
      }
      if (transition_system == "list-graph" || transition_system == "list-tree"){
        if (setOfActions[action] == "NS"){
          double second_score = - DBL_MAX;
          string second_a = setOfActions[current_valid_actions[0]];
          for (unsigned i = 1; i < current_valid_actions.size(); ++i) {
            const string& actionstr=setOfActions[current_valid_actions[i]];
            const char  ac0 = actionstr[0];
            //cerr << actionstr << "-" << adist[current_valid_actions[i]] << endl;
            if (adist[current_valid_actions[i]] > second_score &&
                (ac0 == 'R' || ac0 == 'L')) {
                second_score = adist[current_valid_actions[i]];
                second_a = actionstr;
            }
          }
          //cerr << second_a << endl;
          if (second_a[0] == 'L' || second_a[0] == 'R'){
            int headi = (second_a[0] == 'L' ? bufferi.back() : stacki.back());
            int depi = (second_a[0] == 'L' ? stacki.back() : bufferi.back());
            cand[headi][depi] = second_a.substr(3, second_a.size() - 4);
          }
        }
      }
      //if (!build_training_graph)
      //cerr <<endl<< "gold action: " << setOfActions[action] <<endl;
      ++action_count;
      log_probs.push_back(pick(adiste, action));
      if (transition_system == "swap")
        apply_action(hg,
                   stack_lstm, buffer_lstm, action_lstm, tree_lstm,
                   buffer, bufferi, stack, stacki, results,
                   action, setOfActions, sent, intToWords,
                   cbias, H, D, R, &rootword, dir_graph, word_emb);
      else if (transition_system == "list-graph" || transition_system == "list-tree") {
        results.push_back(action);

        // add current action to action LSTM
        Expression actione = lookup(*hg, p_a, action);
        action_lstm.add_input(actione);

        // get relation embedding from action (TODO: convert to relation from action?)
        Expression relation = lookup(*hg, p_r, action);

        // do action
        const string& actionString=setOfActions[action];
        const char ac = actionString[0];
        const char ac2 = actionString[1];
        //cerr << ac << ac2 << endl;

        if (transition_system == "list-graph" || transition_system == "list-tree"){
            if (ac =='N' && ac2=='S') {  // NO-SHIFT
                assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
                int pass_size = (int)pass.size();
                for (int i = 1; i < pass_size; i++){  //do not move pass_guard
                    stack.push_back(pass.back());
                    stack_lstm.add_input(pass.back());
                    pass.pop_back();
                    pass_lstm.rewind_one_step();
                    stacki.push_back(passi.back());
                    passi.pop_back();
                }
                stack.push_back(buffer.back());
                stack_lstm.add_input(buffer.back());
                buffer.pop_back();
                if (!Opt.USE_BILSTM)
                  buffer_lstm.rewind_one_step();
                stacki.push_back(bufferi.back());
                bufferi.pop_back();
            } else if (ac=='N' && ac2=='R'){
                assert(stacki.size() > 1);
                if (word_rep)
                    (*word_rep)[stacki.back()] = stack.back();
                stack.pop_back();
                stacki.pop_back();
                stack_lstm.rewind_one_step();
            } else if (ac=='N' && ac2=='P'){
                assert(stacki.size() > 1);
                pass.push_back(stack.back());
                pass_lstm.add_input(stack.back());
                passi.push_back(stacki.back());
                stack.pop_back();
                stacki.pop_back();
                stack_lstm.rewind_one_step();
            } else if (ac=='L'){ // LEFT-REDUCE or LEFT-PASS
                assert(stacki.size() > 1 && bufferi.size() > 1);
                Expression dep, head;
                unsigned depi, headi;
                dep = stack.back();
                depi = stacki.back();
                stack.pop_back();
                stacki.pop_back();
                head = buffer.back();
                headi = bufferi.back();
                buffer.pop_back();
                bufferi.pop_back();
                dir_graph[headi][depi] = true; // add this arc to graph
                //dir_graph[headi][depi] = REL_EXIST;
                if (headi == sent.size() - 1) rootword = intToWords.find(sent[depi])->second;
                Expression nlcomposed;
                if (Opt.USE_TREELSTM){
                  vector<unsigned> c = get_children(headi, dir_graph);
                  /*cerr << "children: ";
                  for(int i = 0; i < c.size(); i++)
                    cerr << c[i] << " ";
                  cerr << endl;*/
                  nlcomposed = tree_lstm.add_input(headi, get_children(headi, dir_graph), word_emb[headi]);
                } else{
                  Expression composed = affine_transform({cbias, H, head, D, dep, R, relation});
                  nlcomposed = tanh(composed);
                }
                stack_lstm.rewind_one_step();
                if (!Opt.USE_BILSTM){
                  buffer_lstm.rewind_one_step();
                  buffer_lstm.add_input(nlcomposed);
                }
                buffer.push_back(nlcomposed);
                bufferi.push_back(headi);
                if (ac2 == 'R'){
                    if (word_rep)
                        (*word_rep)[depi] = dep;
                }
                if (ac2 == 'P'){ // LEFT-PASS
                    pass_lstm.add_input(dep);
                    pass.push_back(dep);
                    passi.push_back(depi);
                }
            } else if (ac=='R'){ // RIGHT-SHIFT or RIGHT-PASSA
                assert(stacki.size() > 1 && bufferi.size() > 1);
                Expression dep, head;
                unsigned depi, headi;
                dep = buffer.back();
                depi = bufferi.back();
                buffer.pop_back();
                bufferi.pop_back();
                head = stack.back();
                headi = stacki.back();
                stack.pop_back();
                stacki.pop_back();
                dir_graph[headi][depi] = true; // add this arc to graph
                //dir_graph[headi][depi] = REL_EXIST;
                if (headi == sent.size() - 1) rootword = intToWords.find(sent[depi])->second;
                Expression nlcomposed;
                if (Opt.USE_TREELSTM){
                  vector<unsigned> c = get_children(headi, dir_graph);
                  /*cerr << "children: ";
                  for(int i = 0; i < c.size(); i++)
                    cerr << c[i] << " ";
                  cerr << endl;*/
                  nlcomposed = tree_lstm.add_input(headi, get_children(headi, dir_graph),word_emb[headi]);
                } else{
                  Expression composed = affine_transform({cbias, H, head, D, dep, R, relation});
                  nlcomposed = tanh(composed);
                }
                stack_lstm.rewind_one_step();
                if (!Opt.USE_BILSTM)
                    buffer_lstm.rewind_one_step();
                if (ac2 == 'S'){ //RIGHT-SHIFT
                    stack_lstm.add_input(nlcomposed);
                    stack.push_back(nlcomposed);
                    stacki.push_back(headi);
                    int pass_size = (int)pass.size();
                    for (int i = 1; i < pass_size; i++){  //do not move pass_guard
                        stack.push_back(pass.back());
                        stack_lstm.add_input(pass.back());
                        pass.pop_back();
                        pass_lstm.rewind_one_step();
                        stacki.push_back(passi.back());
                        passi.pop_back();
                    }
                    stack_lstm.add_input(dep);
                    stack.push_back(dep);
                    stacki.push_back(depi);
                }
                else if (ac2 == 'P'){
                    pass_lstm.add_input(nlcomposed);
                    pass.push_back(nlcomposed);
                    passi.push_back(headi);
                    if (!Opt.USE_BILSTM)
                      buffer_lstm.add_input(dep);
                    buffer.push_back(dep);
                    bufferi.push_back(depi);
                }
            }
        }
      }
    }
    if (transition_system == "swap"){
        assert(stack.size() == 2); // guard symbol, root
        assert(stacki.size() == 2);
    }
    assert(buffer.size() == 1); // guard symbol
    assert(bufferi.size() == 1);
    Expression tot_neglogprob = -sum(log_probs);
    assert(tot_neglogprob.pg != nullptr);
    //collect word representation 
    if (word_rep)
        for (unsigned i = 1; i < stack.size(); ++i) // do not collect stack_guard
            (*word_rep)[stacki[i]] = stack[i];
    //collect action representation
    if (act_rep)
        *act_rep = action_lstm.back();
    return results;
  }

  // Applies an action to a ParserState struct
  void LSTMParser::apply_action_to_state(  ComputationGraph* hg,
                             ParserState* ns,
                             unsigned action,
                             const vector<string>& setOfActions,
                             const vector<unsigned>& sent,  // sent with oovs replaced
                             const map<unsigned, std::string>& intToWords,
                             const Expression& cbias,
                             const Expression& H,
                             const Expression& D,
                             const Expression& R,
                             string* rootword,
                             const vector<Expression>& word_emb) {
    apply_action(hg,
                 ns->stack_lstm, ns->buffer_lstm, ns->action_lstm, ns->tree_lstm,
                 ns->buffer, ns->bufferi, ns->stack, ns->stacki, ns->results,
                 action, setOfActions,
                 sent, intToWords,
                 cbias, H, D, R,
                 rootword, ns->graph, word_emb);
    /*apply_action2(hg,
                 ns->stack_lstm, ns->buffer_lstm, ns->action_lstm,
                 ns->buffer, ns->bufferi, ns->stack, ns->stacki, ns->results,
                 action, setOfActions,
                 sent, intToWords,
                 cbias, H, D, R,
                 rootword);*/
  }

  void LSTMParser::apply_action( ComputationGraph* hg,
                   LSTMBuilder& stack_lstm,
                   LSTMBuilder& buffer_lstm,
                   LSTMBuilder& action_lstm,
                   TheirTreeLSTMBuilder& tree_lstm,
                   vector<Expression>& buffer,
                   vector<int>& bufferi,
                   vector<Expression>& stack,
                   vector<int>& stacki,
                   vector<unsigned>& results,
                   unsigned action,
                   const vector<string>& setOfActions,
                   const vector<unsigned>& sent,  // sent with oovs replaced
                   const map<unsigned, std::string>& intToWords,
                   const Expression& cbias,
                   const Expression& H,
                   const Expression& D,
                   const Expression& R,
                   string* rootword,
                   vector<vector<bool>>& graph,
                   const vector<Expression>& word_emb
                   ) {

    // add current action to results
    //cerr << "add current action to results\n";
    results.push_back(action);

    // add current action to action LSTM
    //cerr << "add current action to action LSTM\n";
    Expression actione = lookup(*hg, p_a, action);
    action_lstm.add_input(actione);

    // get relation embedding from action (TODO: convert to relation from action?)
    //cerr << "get relation embedding from action\n";
    Expression relation = lookup(*hg, p_r, action);

    const string &actionString = setOfActions[action];

    const char ac = actionString[0];
    const char ac2 = actionString[1];
    //cerr << ac << ac2 << endl;
    // Execute one of the actions
    if (ac == 'S' && ac2 == 'H') {  // SHIFT
        assert(buffer.size() > 1); // dummy symbol means > 1 (not >= 1)
        stack.push_back(buffer.back());
        stack_lstm.add_input(buffer.back());
        buffer.pop_back();
        if (!Opt.USE_BILSTM)
          buffer_lstm.rewind_one_step();
        stacki.push_back(bufferi.back());
        bufferi.pop_back();
    }
    else if (ac == 'S' && ac2 == 'W') { //SWAP --- Miguel
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)

        //std::cout<<"SWAP: "<<"stack.size:"<<stack.size()<<"\n";

        Expression toki, tokj;
        unsigned ii = 0, jj = 0;
        tokj = stack.back();
        jj = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        toki = stack.back();
        ii = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        buffer.push_back(toki);
        bufferi.push_back(ii);

        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();

        if (!Opt.USE_BILSTM)
          buffer_lstm.add_input(buffer.back());

        stack.push_back(tokj);
        stacki.push_back(jj);

        stack_lstm.add_input(stack.back());

        //stack_lstm.rewind_one_step();
        //buffer_lstm.rewind_one_step();
    }
    else { // LEFT or RIGHT
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)
        assert(ac == 'L' || ac == 'R');
        Expression dep, head;
        unsigned depi = 0, headi = 0;
        (ac == 'R' ? dep : head) = stack.back();
        (ac == 'R' ? depi : headi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        (ac == 'R' ? head : dep) = stack.back();
        (ac == 'R' ? headi : depi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        if (headi == sent.size() - 1) *rootword = intToWords.find(sent[depi])->second;
        // composed = cbias + H * head + D * dep + R * relation
        graph[headi][depi] = true;
        Expression nlcomposed;
        if (Opt.USE_TREELSTM){
            vector<unsigned> c = get_children(headi, graph);
            nlcomposed = tree_lstm.add_input(headi, get_children(headi, graph), word_emb[headi]);
        } else{
            Expression composed = affine_transform({cbias, H, head, D, dep, R, relation});
            nlcomposed = tanh(composed);
        }
        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();
        stack_lstm.add_input(nlcomposed);
        stack.push_back(nlcomposed);
        stacki.push_back(headi);
    }
  }

  //without [treelstm]
  // Applies an action (shift, reduce, etc) to a stack, buffer, and LSTM set
  /*void LSTMParser::apply_action2( ComputationGraph* hg,
                   LSTMBuilder& stack_lstm,
                   LSTMBuilder& buffer_lstm,
                   LSTMBuilder& action_lstm,
                   vector<Expression>& buffer,
                   vector<int>& bufferi,
                   vector<Expression>& stack,
                   vector<int>& stacki,
                   vector<unsigned>& results,
                   unsigned action,
                   const vector<string>& setOfActions,
                   const vector<unsigned>& sent,  // sent with oovs replaced
                   const map<unsigned, std::string>& intToWords,
                   const Expression& cbias,
                   const Expression& H,
                   const Expression& D,
                   const Expression& R,
                   string* rootword) {

    // add current action to results
    //cerr << "add current action to results\n";
    results.push_back(action);

    // add current action to action LSTM
    //cerr << "add current action to action LSTM\n";
    Expression actione = lookup(*hg, p_a, action);
    action_lstm.add_input(actione);

    // get relation embedding from action (TODO: convert to relation from action?)
    //cerr << "get relation embedding from action\n";
    Expression relation = lookup(*hg, p_r, action);

    const string &actionString = setOfActions[action];



    const char ac = actionString[0];
    const char ac2 = actionString[1];
    // Execute one of the actions
    if (ac == 'S' && ac2 == 'H') {  // SHIFT
        assert(buffer.size() > 1); // dummy symbol means > 1 (not >= 1)
        stack.push_back(buffer.back());
        stack_lstm.add_input(buffer.back());
        buffer.pop_back();
        buffer_lstm.rewind_one_step();
        stacki.push_back(bufferi.back());
        bufferi.pop_back();
    }
    else if (ac == 'S' && ac2 == 'W') { //SWAP --- Miguel
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)

        //std::cout<<"SWAP: "<<"stack.size:"<<stack.size()<<"\n";

        Expression toki, tokj;
        unsigned ii = 0, jj = 0;
        tokj = stack.back();
        jj = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        toki = stack.back();
        ii = stacki.back();
        stack.pop_back();
        stacki.pop_back();

        buffer.push_back(toki);
        bufferi.push_back(ii);

        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();


        buffer_lstm.add_input(buffer.back());

        stack.push_back(tokj);
        stacki.push_back(jj);

        stack_lstm.add_input(stack.back());

        //stack_lstm.rewind_one_step();
        //buffer_lstm.rewind_one_step();
    }
    else { // LEFT or RIGHT
        assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)
        assert(ac == 'L' || ac == 'R');
        Expression dep, head;
        unsigned depi = 0, headi = 0;
        (ac == 'R' ? dep : head) = stack.back();
        (ac == 'R' ? depi : headi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        (ac == 'R' ? head : dep) = stack.back();
        (ac == 'R' ? headi : depi) = stacki.back();
        stack.pop_back();
        stacki.pop_back();
        if (headi == sent.size() - 1) *rootword = intToWords.find(sent[depi])->second;
        // composed = cbias + H * head + D * dep + R * relation
        Expression composed = affine_transform({cbias, H, head, D, dep, R, relation});
        Expression nlcomposed = tanh(composed);
        stack_lstm.rewind_one_step();
        stack_lstm.rewind_one_step();
        stack_lstm.add_input(nlcomposed);
        stack.push_back(nlcomposed);
        stacki.push_back(headi);
    }
}*/

  // run beam search
vector<unsigned> LSTMParser::log_prob_parser_beam(ComputationGraph *hg, 
                    const vector<unsigned> &raw_sent,
                    const vector<unsigned> &sent, 
                    const vector<unsigned> &sentPos,
                    const vector<unsigned> &correct_actions, 
                    const vector<string> &setOfActions,
                    const map<unsigned, std::string> &intToWords, 
                    double *right, 
                    unsigned beam_size,
                    DataGatherer &dg) {
        //for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
        //cerr << endl;

        //cerr << "\nstarting with memory usage: " << getMemoryUsage() << "\n";
        //const vector<string> setOfActions = corpus.actions;
        //const map<unsigned, std::string> intToWords = corpus.intToWords;

        auto overall_start = std::chrono::high_resolution_clock::now();

        vector<unsigned> results;
        const bool build_training_graph = correct_actions.size() > 0;

        stack_lstm.new_graph(*hg);
        stack_lstm.start_new_sequence();
        //buffer_lstm.new_graph(*hg);
        //buffer_lstm.start_new_sequence();
        action_lstm.new_graph(*hg);
        action_lstm.start_new_sequence();

        Expression fwB;
        Expression bwB;
        if (Opt.USE_BILSTM){
            buffer_bilstm.new_graph(hg); // [bilstm] start_new_sequence is implemented in add_input
            fwB = parameter(*hg, p_fwB); // [bilstm]
            bwB = parameter(*hg, p_bwB); // [bilstm]
        }else{
            buffer_lstm.new_graph(*hg);
            buffer_lstm.start_new_sequence();
        }
        if (Opt.USE_TREELSTM){
            tree_lstm.new_graph(*hg); // [treelstm]
            tree_lstm.start_new_sequence(); // [treelstm]
            tree_lstm.initialize_structure(sent.size()); // [treelstm]
        }
        // variables in the computation graph representing the parameters
        Expression pbias = parameter(*hg, p_pbias);
        Expression H = parameter(*hg, p_H);
        Expression D = parameter(*hg, p_D);
        Expression R = parameter(*hg, p_R);
        Expression cbias = parameter(*hg, p_cbias);
        Expression S = parameter(*hg, p_S);
        Expression B = parameter(*hg, p_B);
        Expression A = parameter(*hg, p_A);
        Expression ib = parameter(*hg, p_ib);
        Expression w2l = parameter(*hg, p_w2l);
        Expression p2l;
        if (Opt.USE_POS)
            p2l = parameter(*hg, p_p2l);
        Expression t2l;
        if (use_pretrained)
            t2l = parameter(*hg, p_t2l);
        Expression p2a = parameter(*hg, p_p2a);
        Expression abias = parameter(*hg, p_abias);
        Expression action_start = parameter(*hg, p_action_start);

        action_lstm.add_input(action_start);
        vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
        vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
        // precompute buffer representation from left to right
        vector<Expression> word_emb(sent.size()); // [treelstm] store original word representation emb[i] for sent[i]

        for (unsigned i = 0; i < sent.size(); ++i) {
            assert(sent[i] < System_size.VOCAB_SIZE);
            Expression w=lookup(*hg, p_w, sent[i]);

            Expression i_i;
            if (Opt.USE_POS) {
                Expression p = lookup(*hg, p_p, sentPos[i]);
                i_i = affine_transform({ib, w2l, w, p2l, p});
            } else {
                i_i = affine_transform({ib, w2l, w});
            }
            if (use_pretrained && pretrained.count(raw_sent[i])) {
                Expression t = const_lookup(*hg, p_t, raw_sent[i]);
                i_i = affine_transform({i_i, t2l, t});
            }
            buffer[sent.size() - i] = rectify(i_i);
            bufferi[sent.size() - i] = i;
            if (Opt.USE_TREELSTM)
                word_emb[i] = buffer[sent.size() - i];
        }
        if (Opt.USE_TREELSTM){
            vector<unsigned> h;
            for (int i = 0; i < sent.size(); i++)
                tree_lstm.add_input(i, h, word_emb[i]);
        }

        // dummy symbol to represent the empty buffer
        buffer[0] = parameter(*hg, p_buffer_guard);
        bufferi[0] = -999;
        std::vector<BidirectionalLSTMLayer::Output> bilstm_outputs;
        if (Opt.USE_BILSTM){
            buffer_bilstm.add_inputs(hg, buffer); 
            buffer_bilstm.get_outputs(hg, bilstm_outputs); // [bilstm] output of bilstm for buffer, first is fw, second is bw
        }else{
            for (auto& b : buffer)
                buffer_lstm.add_input(b);
        }

        vector<Expression> stack;  // variables representing subtree embeddings
        vector<int> stacki; // position of words in the sentence of head of subtree
        stack.push_back(parameter(*hg, p_stack_guard));
        stacki.push_back(-999); // not used for anything
        // drive dummy symbol on stack through LSTM
        stack_lstm.add_input(stack.back());

        //
        // End of "setup code", below this is beam search code
        //

        auto step_start = std::chrono::high_resolution_clock::now();

        int newcount = 0;
        int delcount = 0;

        // initialize structures for beam search
        ParserState* init = new ParserState(); newcount ++;

        vector<bool> v;
        for (int i = 0; i < (int)sent.size(); ++i)
            v.push_back(false);
        for (int i = 0; i < (int)sent.size(); ++i)
            init->graph.push_back(v);
        init->stack_lstm = stack_lstm;
        init->buffer_lstm = buffer_lstm;
        init->action_lstm = action_lstm;
        if (Opt.USE_TREELSTM){
            init->tree_lstm = tree_lstm;
        }
        init->buffer = buffer;
        init->bufferi = bufferi;
        init->stack = stack;
        init->stacki = stacki;
        init->results = results;
        init->score = 0;
        init->gold = true;
        if (init->stacki.size() ==1 && init->bufferi.size() == 1) { assert(!"bad0"); }

        vector<ParserState*> ongoing; // represents the currently-active beams
        ongoing.push_back(init);
        vector<StepSelect> next_beams; // represents the "next" set of beams, to be used when all current beams are exhausted
        vector<ParserState*> completed; // contains beams that have parsed the whole sentence
        unordered_set<ParserState*> need_to_delete;
        need_to_delete.insert(init);

        auto loop_start = std::chrono::high_resolution_clock::now();
  
        // double beam_acceptance_percentage;
        // if (DYNAMIC_BEAM) { beam_acceptance_percentage = log((100-beam_size)/100.0); beam_size = 32; } // 32 is maximum beams for dynamic
        unsigned active_beams = beam_size; // counts the number of incomplete beams we still need to process
        string rootword;
        ParserState* gold_parse = init;
        unsigned action_count = 0;  // incremented at each prediction
        bool full_gold_found = false;
        vector<Expression> log_probs;
        vector<Expression> log_zlocals;
        vector<Expression> rhos;
        while (completed.size() < beam_size){
            if (ongoing.size() == 0) { // if we've run out of beams in the current step, start on the next one
                auto step_end = std::chrono::high_resolution_clock::now();
                double dur = std::chrono::duration<double, std::milli>(step_end - step_start).count();
                step_start = step_end;

                if (next_beams.size() == 0) {
                    // Sometimes, we have completed all the beams we can, but we set the beam size too high, and there
                    // just aren't enough unique moves to complete more. In that case, we are just done.
                    break;
                }

                // Move the next set to be the current set
                for (StepSelect st : next_beams) {
                    // Create a new ParserState, copying the current one
                    ParserState* ns = new ParserState(); newcount++;
                    need_to_delete.insert(ns); // this prevents memory leaks
                    *ns = *(st.source);

                    // Update the score
                    ns->score += st.action.score;

                    // update the goldness
                    if (build_training_graph && (!ns->gold || st.action.val != correct_actions[action_count])) ns->gold = false;

                    // action_log_prob = pick(adist, action)
                    ns->log_probs.push_back(st.action.log_prob);
                    if (Opt.GLOBAL_LOSS) {
                        ns->log_zlocals.push_back(st.action.log_zlocal);
                        ns->rhos.push_back(st.action.rho);
                    }
                    // do action
                    apply_action_to_state(hg, ns, st.action.val,
                                          setOfActions, sent, intToWords,
                                          cbias, H, D, R, &rootword, word_emb);
                    ongoing.push_back(ns);
                }
                next_beams.clear();
                ++action_count;

                // if we have reference actions (for training), and are doing early-update,
                // check whether we need to cut off parsing of the sentence
                if (build_training_graph) {

                    bool gold_in_beam = full_gold_found;
                    for (ParserState* ps : ongoing) {
                        if (ps->gold) {
                            gold_in_beam = true;
                            break;
                        }
                    }
                    if (!gold_in_beam) {
                        Action gold_action = gold_parse->next_gold_action;

                        gold_parse->score += gold_action.score;
                        // action_log_prob = pick(adist, action)
                        gold_parse->log_probs.push_back(gold_action.log_prob);
                        if (Opt.GLOBAL_LOSS) {
                            gold_parse->log_zlocals.push_back(gold_action.log_zlocal);
                            gold_parse->rhos.push_back(gold_action.rho);
                        }

                        // is this necessary?
                        apply_action_to_state(hg, gold_parse, gold_action.val,
                                              setOfActions, sent, intToWords,
                                              cbias, H, D, R, &rootword, word_emb);
                        break;
                    }
                }
            } 

            while (ongoing.size() != 0) {
                // get the state of a beam, and remove that beam from ongoing (because it has been processed)
                ParserState *cur = ongoing.back();
                need_to_delete.insert(cur); // this prevents memory leaks
                ongoing.pop_back();
                //cerr << "sc2 " << ongoing.top()->score << "\n";

                // check whether the current beam is completed
                if (cur->stack.size() == 2 && cur->buffer.size() == 1) {
                    completed.push_back(cur);
                    if (cur->gold) {
                        gold_parse = cur;
                        full_gold_found = true;
                    }
                    --active_beams;
                    if (completed.size() == beam_size)
                        break; // we have completed all the beams we need, so just end here
                    continue;
                }
                // Since we have now confirmed that the beam is not complete, we want to generate all possible actions to
                // take from here, and keep the best states for the next beam set
                dg.decisions_made++;
                getNextBeamsArgs nba{setOfActions,p2a,pbias,abias,S,B,A,fwB,bwB,build_training_graph,
                                      correct_actions,action_count,bilstm_outputs};
                vector<StepSelect> potential_next_beams;
                getNextBeams(cur, &potential_next_beams,
                                 hg,
                                 nba,
                                 gold_parse);
                next_beams.insert(next_beams.end(), potential_next_beams.begin(), potential_next_beams.end());
            }
            // cull down next_beams to just keep the best beams
            // keep the next_beams sorted
            sort(next_beams.begin(), next_beams.end(), StepSelectCompare());
            while (next_beams.size() > active_beams) {
                next_beams.pop_back();
            }
        } // while (completed.size() < beam_size)
        auto got_answers = std::chrono::high_resolution_clock::now();
        // if we are training, just use the gold one
        if (build_training_graph) {
            stack_lstm = gold_parse->stack_lstm;
            buffer_lstm = gold_parse->buffer_lstm;
            action_lstm = gold_parse->action_lstm;
            tree_lstm = gold_parse->tree_lstm; //[treelstm]
            stack = gold_parse->stack;
            stacki = gold_parse->stacki;
            buffer = gold_parse->buffer;
            bufferi = gold_parse->bufferi;
            results = gold_parse->results;
            log_probs = gold_parse->log_probs;
            if (Opt.GLOBAL_LOSS) {
                log_zlocals = gold_parse->log_zlocals;
                rhos = gold_parse->rhos;
            }
            // Count how many actions we got right
            assert(results.size() <= correct_actions.size());
            for (unsigned i = 0; i < results.size(); i++) {
                if (correct_actions[i] == results[i]) { (*right)++; }
            }
        } else { // if we don't have answers, just take the results from the best beam
            sort(completed.begin(), completed.end(), ParserStatePointerCompare());

            stack_lstm = completed.front()->stack_lstm;
            buffer_lstm = completed.front()->buffer_lstm;
            action_lstm = completed.front()->action_lstm;
            tree_lstm = completed.front()->tree_lstm; //[treelstm]
            stack = completed.front()->stack;
            stacki = completed.front()->stacki;
            buffer = completed.front()->buffer;
            bufferi = completed.front()->bufferi;
            results = completed.front()->results;
            log_probs = completed.front()->log_probs;
            if (Opt.GLOBAL_LOSS) {
                log_zlocals = completed.front()->log_zlocals;
                rhos = completed.front()->rhos;
            }

            assert(stack.size() == 2); // guard symbol, root
            assert(stacki.size() == 2);
            assert(buffer.size() == 1); // guard symbol
            assert(bufferi.size() == 1);

            auto overall_end = std::chrono::high_resolution_clock::now();
        }

        Expression intermediate_loss;
        if (Opt.GLOBAL_LOSS && build_training_graph) {
            // Global loss from Andor et al. 2016
            // NOTE - this did not end up working
            vector<Expression> beam_sum_rhos;
            //vector<Expression> beam_exp_sum_log_probs;
            //vector<Expression> beam_sum_log_probs; // sum(beam_sum_log_pLI) = log(pL)
            //vector<Expression> beam_sum_log_probs2; // sum(beam_sum_log_pLI) = log(pL)

            if (completed.size() < beam_size) { // Bj, all ongoing beams and the gold beam
                for (ParserState *ps : ongoing) { beam_sum_rhos.push_back(sum(ps->rhos)); }
                beam_sum_rhos.push_back(sum(rhos)); // gold beam

                /*for (ParserState *ps : ongoing) { beam_exp_sum_log_probs.push_back(exp(sum(ps->log_probs))); }
                beam_exp_sum_log_probs.push_back(exp(sum(log_probs))); // gold beam
    
                for (ParserState *ps : ongoing) { beam_sum_log_probs.push_back(sum(ps->log_probs)); }
                beam_sum_log_probs.push_back(sum(log_probs)); // gold beam

                for (ParserState *ps : ongoing) {
                    vector<Expression> log_pLIs;
                    for (unsigned act_i = 0; act_i < ps->rhos.size(); act_i++) {
                        log_pLIs.push_back(ps->rhos[act_i] - ps->log_zlocals[act_i]);
                    }
                    beam_sum_log_probs2.push_back(sum(log_pLIs));
                }
                vector<Expression> log_pLIs;
                for (unsigned act_i = 0; act_i < rhos.size(); act_i++) {
                     log_pLIs.push_back(rhos[act_i] - log_zlocals[act_i]);
                }
                beam_sum_log_probs2.push_back(sum(log_pLIs)); // gold beam */
            } else { // Bn, set of completed beams
                assert(completed.size() == beam_size);
                for (ParserState* ps : completed) { beam_sum_rhos.push_back(sum(ps->rhos)); }
                /*for (ParserState* ps : completed) { beam_exp_sum_log_probs.push_back(exp(sum(ps->log_probs))); }
                for (ParserState* ps : completed) { beam_sum_log_probs.push_back(sum(ps->log_probs)); }
                for (ParserState* ps : completed) {
                    vector<Expression> log_pLIs;
                    for (unsigned act_i = 0; act_i < ps->rhos.size(); act_i++) {
                        log_pLIs.push_back(ps->rhos[act_i] - ps->log_zlocals[act_i]);
                    }
                    beam_sum_log_probs2.push_back(sum(log_pLIs));
                }*/
            }

//            cerr << "rhos:       \t"; for (Expression rho : rhos) { cerr << rho.value() << "\t";}  cerr << "\n";
//            cerr << "beam zg:    \t"; for (Expression beam_zglobal : beam_zglobals) { cerr << beam_zglobal.value() << "\n\t\t";}  cerr << "\n";
//
//            cerr << "-sum(rhos) ?= -sum(log_probs) - sum(log_zlocals): \n";
//            cerr << (-sum(rhos)).value() << " ?= " << (-sum(log_probs)-sum(log_zlocals)).value() << " (" << (-sum(log_probs)).value() << " + " << (-sum(log_zlocals)).value() << ")\n";
//
//            cerr << (-sum(rhos)).value() <<  " + " << logsumexp(beam_zglobals).value() << "\n";

            cerr << std::setprecision(10);

            /*vector<Expression> log_pLIs;            
            vector<Expression> lil_rhos;
            vector<Expression> lil_bslp;
            for (unsigned act_i = 0; act_i < rhos.size(); act_i++) {
                log_pLIs.push_back(rhos[act_i] - log_zlocals[act_i]);
            }*/
            intermediate_loss = -sum(rhos) + logsumexp(beam_sum_rhos);

      
        } else {
            intermediate_loss = -sum(log_probs);
        }

        // prevents memory leaks
        ongoing.clear();
        next_beams.clear();
        completed.clear();
        for (ParserState* ps: need_to_delete) {delete ps; delcount++;}
        need_to_delete.clear();


        dg.sentences_parsed++;
        Expression tot_neglogprob;
        tot_neglogprob = intermediate_loss;
        assert(tot_neglogprob.pg != nullptr);
        return results;
  }

  void LSTMParser::process_headless_search_all(const vector<unsigned>& sent, const vector<unsigned>& sentPos, 
                                                        const vector<string>& setOfActions, vector<Expression>& word_rep, 
                                                        Expression& act_rep, int n, int sent_len, int dir, map<int, double>* scores, 
                                                        map<int, string>* rels){
        for (int i = n + dir; i >= 0 && i < sent_len; i += dir){
            int s0 = (dir > 0 ? n : i);
            int b0 = (dir > 0 ? i : n);
            double score;
            string rel;
            ComputationGraph cg;
            get_best_label(sent, sentPos, &cg, setOfActions, s0, b0, word_rep , act_rep, sent_len, dir, &score, &rel);
            (*scores)[i] = score;
            (*rels)[i] = rel;
            //cerr << "search all n: " << n << " i: " << i << "rel: " << rel << endl;
        }
  } 

  void LSTMParser::getNextBeams(ParserState* cur, vector<StepSelect>* potential_next_beams,
                          ComputationGraph* hg, const getNextBeamsArgs& args,
                          ParserState*& gold_parse){
    const vector<string>& setOfActions = args.setOfActions;
    const Expression& p2a = args.p2a;
    const Expression& pbias = args.pbias;
    const Expression& abias = args.abias;
    const Expression& S = args.S;
    const Expression& B = args.B;
    const Expression& A = args.A;
    const bool& build_training_graph = args.build_training_graph;
    const vector<unsigned>& correct_actions = args.correct_actions;
    const int& action_count = args.action_count;

    //const std::vector<BidirectionalLSTMLayer::Output>& bilstm_outputs = args.bilstm_outputs;
    //const Expression& fwB = args.fwB;
    //const Expression& bwB = args.bwB;
    int idx = 0;
    if (Opt.USE_BILSTM){
        unsigned sent_len = cur->graph.size();
        if (cur->bufferi.size() > 1)
            idx = sent_len - cur->bufferi.back();
    }
    // get list of possible actions for the current parser state
    vector<unsigned> current_valid_actions;
    for (auto a: possible_actions) {
        if (IsActionForbidden2(setOfActions[a], cur->buffer.size(), cur->stack.size(), cur->stacki))
            continue;
        current_valid_actions.push_back(a);
    }

    // p_t = pbias + S * slstm + B * blstm + A * almst
    //Expression p_t = affine_transform({pbias, S, cur->stack_lstm.back(), B, cur->buffer_lstm.back(), A, cur->action_lstm.back()});
    
    Expression p_t;
    if (Opt.USE_BILSTM){
        //cerr << "bilstm: " << bilstm_outputs.size() << " id: " 
        //<< sent.size() - bufferi.back() << " bufferi: " << bufferi.back() << endl;
        Expression fwbuf,bwbuf;
        fwbuf = args.bilstm_outputs[idx].first - args.bilstm_outputs[1].first;
        bwbuf = args.bilstm_outputs[1].second - args.bilstm_outputs[idx].second;
        // [bilstm] p_t = pbias + S * slstm + fwB * blstm_fw + bwB * blstm_bw + A * almst
        p_t = affine_transform({pbias, S, cur->stack_lstm.back(), args.fwB, fwbuf, args.bwB, bwbuf, 
                                A, cur->action_lstm.back()});
    }else{
        p_t = affine_transform({pbias, S, cur->stack_lstm.back(), B, cur->buffer_lstm.back(), 
                                A, cur->action_lstm.back()});
    }
    Expression nlp_t = rectify(p_t);
    // r_t = abias + p2a * nlp
    Expression r_t = affine_transform({abias, p2a, nlp_t});
    // adist = log_softmax(r_t, current_valid_actions)
    Expression adiste = log_softmax(r_t, current_valid_actions);
    vector<float> adist = as_vector(hg->incremental_forward(adiste));

    Expression log_zlocal;
    if (Opt.GLOBAL_LOSS) { // not used in experiments
        vector<Expression> intermediate;
        for (unsigned valid_action_loc : current_valid_actions) { intermediate.push_back(pick(r_t, valid_action_loc)); }
        log_zlocal = logsumexp(intermediate);
    }
    for (unsigned i = 0; i < current_valid_actions.size(); ++i) {
        // For each action, its value is equal to the current state's value, plus the value of the action
        double total_score = cur->score + adist[current_valid_actions[i]];
        if (Opt.GLOBAL_LOSS) {
            total_score = cur->score + *(pick(r_t, current_valid_actions[i]).value().v);
        }
        //cerr << "filling\n";
        Action act;
        act.score = adist[current_valid_actions[i]];
        act.val = current_valid_actions[i];
        act.log_prob = pick(adiste, act.val);
        if (Opt.GLOBAL_LOSS) {
            act.log_zlocal = log_zlocal;
            act.rho = pick(r_t, act.val);
            act.score = *(act.rho.value().v);
        }
        StepSelect next_step;
        next_step.source = cur;
        next_step.action = act;
        next_step.total_score = total_score;

        // if it is gold, give the gold act
        if (build_training_graph && cur->gold) {
            Action gold_act;
            gold_act.score = adist[correct_actions[action_count]];
            gold_act.val = correct_actions[action_count];
            gold_act.log_prob = pick(adiste, gold_act.val);
            if (Opt.GLOBAL_LOSS) {
                gold_act.log_zlocal = log_zlocal;
                gold_act.rho = pick(r_t, gold_act.val);
                gold_act.score = *(gold_act.rho.value().v);
            }
            gold_parse = cur;
            gold_parse->next_gold_action = gold_act;
        }
        potential_next_beams->push_back(next_step);
    }
}

// without [bilstm]
/*void LSTMParser::getNextBeams2(ParserState* cur, vector<StepSelect>* potential_next_beams,
                          ComputationGraph* hg, const getNextBeamsArgs& args,
                          ParserState*& gold_parse){

    const vector<string>& setOfActions = args.setOfActions;
    const Expression& p2a = args.p2a;
    const Expression& pbias = args.pbias;
    const Expression& abias = args.abias;
    const Expression& S = args.S;
    const Expression& B = args.B;
    const Expression& A = args.A;
    const bool& build_training_graph = args.build_training_graph;
    const vector<unsigned>& correct_actions = args.correct_actions;
    const int& action_count = args.action_count;

    // get list of possible actions for the current parser state
    vector<unsigned> current_valid_actions;
    for (auto a: possible_actions) {
        if (IsActionForbidden2(setOfActions[a], cur->buffer.size(), cur->stack.size(), cur->stacki))
            continue;
        current_valid_actions.push_back(a);
    }

    // p_t = pbias + S * slstm + B * blstm + A * almst
    Expression p_t = affine_transform({pbias, S, cur->stack_lstm.back(), B, cur->buffer_lstm.back(), A, cur->action_lstm.back()});
    Expression nlp_t = rectify(p_t);
    // r_t = abias + p2a * nlp
    Expression r_t = affine_transform({abias, p2a, nlp_t});
    // adist = log_softmax(r_t, current_valid_actions)
    Expression adiste = log_softmax(r_t, current_valid_actions);
    vector<float> adist = as_vector(hg->incremental_forward(adiste));

    Expression log_zlocal;
    if (Opt.GLOBAL_LOSS) { // not used in experiments
        vector<Expression> intermediate;
        for (unsigned valid_action_loc : current_valid_actions) { intermediate.push_back(pick(r_t, valid_action_loc)); }
        log_zlocal = logsumexp(intermediate);
    }
    for (unsigned i = 0; i < current_valid_actions.size(); ++i) {
        // For each action, its value is equal to the current state's value, plus the value of the action
        double total_score = cur->score + adist[current_valid_actions[i]];
        if (Opt.GLOBAL_LOSS) {
            total_score = cur->score + *(pick(r_t, current_valid_actions[i]).value().v);
        }
        //cerr << "filling\n";
        Action act;
        act.score = adist[current_valid_actions[i]];
        act.val = current_valid_actions[i];
        act.log_prob = pick(adiste, act.val);
        if (Opt.GLOBAL_LOSS) {
            act.log_zlocal = log_zlocal;
            act.rho = pick(r_t, act.val);
            act.score = *(act.rho.value().v);
        }
        StepSelect next_step;
        next_step.source = cur;
        next_step.action = act;
        next_step.total_score = total_score;

        // if it is gold, give the gold act
        if (build_training_graph && cur->gold) {
            Action gold_act;
            gold_act.score = adist[correct_actions[action_count]];
            gold_act.val = correct_actions[action_count];
            gold_act.log_prob = pick(adiste, gold_act.val);
            if (Opt.GLOBAL_LOSS) {
                gold_act.log_zlocal = log_zlocal;
                gold_act.rho = pick(r_t, gold_act.val);
                gold_act.score = *(gold_act.rho.value().v);
            }
            gold_parse = cur;
            gold_parse->next_gold_action = gold_act;
        }
        potential_next_beams->push_back(next_step);
    }
}*/

void LSTMParser::get_best_label(const vector<unsigned>& sent, const vector<unsigned>& sentPos, 
                                    ComputationGraph* hg, const vector<string>& setOfActions, 
                                    int s0, int b0, vector<Expression>& word_rep, Expression& act_rep, int sent_size, 
                                    int dir, double *score, string *rel) {
    char prefix = (dir > 0 ? 'L' : 'R');
    //init graph connecting vector
    //vector<bool> dir_graph[sent_size]; // store the connection between words in sent
    vector<vector<bool>> dir_graph;
    //vector<bool> v;
    vector<bool> v;
    for (int i = 0; i < sent_size; i++){
        //v.push_back(false);
        v.push_back(false);
    }
    for (int i = 0; i < sent_size; i++){
        dir_graph.push_back(v);
    }
    stack_lstm.new_graph(*hg);
    buffer_lstm.new_graph(*hg);
    pass_lstm.new_graph(*hg);
    action_lstm.new_graph(*hg);
    stack_lstm.start_new_sequence();
    buffer_lstm.start_new_sequence();
    pass_lstm.start_new_sequence();
    action_lstm.start_new_sequence();
    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression P = parameter(*hg, p_P);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (Opt.USE_POS)
      p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (use_pretrained)
      t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    action_lstm.add_input(action_start);

    //action_lstm.add_input(act_rep);
    vector<Expression> buffer;  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi;
    Expression w =lookup(*hg, p_w, sent[b0]);

    vector<Expression> args = {ib, w2l, w}; // learn embeddings
    if (Opt.USE_POS) { // learn POS tag?
        Expression p = lookup(*hg, p_p, sentPos[b0]);
        args.push_back(p2l);
        args.push_back(p);
    }
    if (use_pretrained && pretrained.count(sent[b0])) {  // include fixed pretrained vectors?
        Expression t = const_lookup(*hg, p_t, sent[b0]);
        args.push_back(t2l);
        args.push_back(t);
    }
    //buffer[] = ib + w2l * w + p2l * p + t2l * t

    buffer.push_back(parameter(*hg, p_buffer_guard));
    buffer.push_back(rectify(affine_transform(args)));
    bufferi.push_back(-999);
    bufferi.push_back(b0);
    for (auto& b : buffer)
        buffer_lstm.add_input(b);

    
    vector<Expression> pass; //variables reperesenting embedding in pass buffer
    pass.push_back(parameter(*hg, p_pass_guard));
    pass_lstm.add_input(pass.back());

    args = {ib, w2l, w}; // learn embeddings
    if (Opt.USE_POS) { // learn POS tag?
        Expression p = lookup(*hg, p_p, sentPos[s0]);
        args.push_back(p2l);
        args.push_back(p);
    }
    if (use_pretrained && pretrained.count(sent[s0])) {  // include fixed pretrained vectors?
        Expression t = const_lookup(*hg, p_t, sent[s0]);
        args.push_back(t2l);
        args.push_back(t);
    }

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki;
    stacki.push_back(-999);
    stacki.push_back(s0);
    stack.push_back(parameter(*hg, p_stack_guard));
    stack_lstm.add_input(stack.back());
    stack.push_back(rectify(affine_transform(args)));
    stack_lstm.add_input(stack.back());

    // get list of possible actions for the current parser state
    vector<unsigned> current_valid_actions;
    for (auto a: possible_actions) {
        //cerr << " " << setOfActions[a]<< " ";
        if (IsActionForbidden(setOfActions[a], buffer.size(), stack.size(), sent.size() - 1, dir_graph, stacki, bufferi))
            continue;
        //cerr << " <" << setOfActions[a] << "> ";
        current_valid_actions.push_back(a);
    }
    // p_t = pbias + S * slstm + P * plstm + B * blstm + A * almst
    Expression p_t = affine_transform({pbias, S, stack_lstm.back(), P, pass_lstm.back(), B, buffer_lstm.back(), A, action_lstm.back()});
    Expression nlp_t = rectify(p_t);
    // r_t = abias + p2a * nlp
    Expression r_t = affine_transform({abias, p2a, nlp_t});

    // adist = log_softmax(r_t, current_valid_actions)
    Expression adiste = log_softmax(r_t, current_valid_actions);
    vector<float> adist = as_vector(hg->incremental_forward(adiste));
    double second_score = - DBL_MAX;
    string second_a = REL_NULL;
    for (unsigned i = 1; i < current_valid_actions.size(); ++i) {
        const string& actionstr=setOfActions[current_valid_actions[i]];
        const char  ac0 = actionstr[0];
        //cerr << actionstr << "-" << adist[current_valid_actions[i]] << endl;
        if (adist[current_valid_actions[i]] > second_score &&
                ac0 == prefix) {
            second_score = adist[current_valid_actions[i]];
            second_a = actionstr;
        }
    }
    // do action
    *score = second_score;
    *rel = second_a;
  }

int LSTMParser::process_headless(vector<vector<string>>& hyp, vector<vector<string>>& cand, vector<Expression>& word_rep, 
                                    Expression& act_rep, const vector<unsigned>& sent, const vector<unsigned>& sentPos){
    //cerr << "process headless" << endl;
    const vector<string>& setOfActions = corpus.actions;
    int root = hyp.size() - 1;
    int miss_head_num = 0;
    bool has_head_flag = false;
    int head; // for tree
    for (unsigned i = 0; i < (hyp.size() - 1); ++i){
        has_head_flag = false;
        head = 0; // for tree
        for (unsigned j = 0; j < hyp.size(); ++j){
            if (hyp[j][i] != REL_NULL){
                has_head_flag = true;
                head ++;
            }
        }
        if (transition_system == "tree" && head > 1){
            cerr << "multi head!" << endl;
        }

        if (!has_head_flag){
            miss_head_num ++;
            // use candidate relations
            for (unsigned j = 0; j < cand.size(); ++j){
                if (cand[j][i] != REL_NULL && !has_path_to(i, j, hyp)){
                    hyp[j][i] = cand[j][i];
                    miss_head_num --;
                    has_head_flag = true;
                    break;
                }
            }
            // recompute
            if (!has_head_flag){
                //count root
                int root_num = 0;
                for (unsigned q = 0; q < hyp.size() - 1; ++q)
                if (hyp[root][q] != REL_NULL)
                    root_num ++;

                map<int, double> scores;
                map<int, string> rels;
                process_headless_search_all(sent, sentPos, setOfActions, word_rep, act_rep, i, (int)(hyp.size()), 1, &scores, &rels);
                process_headless_search_all(sent, sentPos, setOfActions, word_rep, act_rep, i, (int)(hyp.size()), -1, &scores, &rels);
                if (root_num >0)
                    scores[root] = -DBL_MAX;
                double opt_score = -DBL_MAX;
                int opt_head = -1;
                for (int k = 0; k < (int)i; ++k){
                    if (scores[k] > opt_score
                            && !has_path_to(i, k, hyp)) { // no cycle
                        opt_head = k;
                        opt_score = scores[k];
                    }
                }
                for (int k = (int)i + 1; k < (int)(hyp.size()); ++k){
                    if (scores[k] > opt_score
                            && !has_path_to(i, k, hyp)) { // no cycle
                        opt_head = k;
                        opt_score = scores[k];
                    }
                }
                if (opt_head != -1){
                    //cerr << rels[opt_head] << endl;
                    hyp[opt_head][i] = rels[opt_head].substr(3, rels[opt_head].size() - 4);
                    miss_head_num --;
                }
                /*else{ // show error information
                    cerr << "opt_score " << opt_score <<" opt_head " << opt_head << endl;
                    for (int k = 0; k < (int)i; ++k){
                        cerr << "head: " << k << " score: " << scores[k] << " rel: " << rels[k] << " path: " << has_path_to(i, k, hyp) << endl; 
                    }
                    for (int k = i + 1; k < (int)(hyp.size() - 1); ++k){
                        cerr << "head: " << k << " score: " << scores[k] << " rel: " << rels[k] << " path: " << has_path_to(i, k, hyp) << endl; 
                    }
                }*/
            } // recomput*/
        }
    } // for
    int root_num = 0;
    for (unsigned i = 0; i < hyp.size() - 1; ++i)
        if (hyp[root][i] != REL_NULL)
            root_num ++;
    if (root_num != 1){
        cerr << "In Semantic Dependency Parser : Root Error: " << root_num << endl;
      }
    //cerr << "miss_head_num: " << miss_head_num << endl;
    return miss_head_num;
}

void LSTMParser::signal_callback_handler(int /* signum */) {
  if (requested_stop) {
    cerr << "\nReceived SIGINT again, quitting.\n";
    _exit(1);
  }
  cerr << "\nReceived SIGINT terminating optimization early...\n";
  requested_stop = true;
}

void LSTMParser::train(const std::string fname, const unsigned unk_strategy, 
                        const double unk_prob) {
    requested_stop = false;
    signal(SIGINT, signal_callback_handler);
    unsigned status_every_i_iterations = 100;
    double best_LF = 0;
    bool softlinkCreated = false;
    SimpleSGDTrainer sgd(model);
    sgd.eta_decay = 0.08;
    std::vector<unsigned> order(corpus.nsentences);
    for (unsigned i = 0; i < corpus.nsentences; ++i)
      order[i] = i;
    double tot_seen = 0;
    status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
    unsigned si = corpus.nsentences;
    cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
    unsigned trs = 0;
    unsigned prd = 0; // number of predicted actions (in case of early update)
    double right = 0;
    double llh = 0;
    bool first = true;
    int iter = -1;
    //time_t time_start = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    time_t time_start = time(NULL);
    std::string t_s(asctime(localtime(&time_start))); 
    //cerr << "TRAINING STARTED AT: " << asctime(localtime(&time_start)) << endl;
    cerr << "TRAINING STARTED AT: " << t_s.substr(0, t_s.size() - 1) << endl;
    while(!requested_stop) {
      ++iter;
      for (unsigned sii = 0; sii < status_every_i_iterations; ++sii) {
          if (si == corpus.nsentences) {
             si = 0;
             if (first) { first = false; } else { sgd.update_epoch(); }
             cerr << "**SHUFFLE\n";
             random_shuffle(order.begin(), order.end());
          }
          tot_seen += 1;
          const std::vector<unsigned>& sentence=corpus.sentences[order[si]];
          std::vector<unsigned> tsentence=sentence;
          if (unk_strategy == 1) {
             for (auto& w : tsentence)
               if (singletons.count(w) && dynet::rand01() < unk_prob) w = kUNK;
          }
          const std::vector<unsigned>& sentencePos=corpus.sentencesPos[order[si]]; 
          const std::vector<unsigned>& actions=corpus.correct_act_sent[order[si]];
          ComputationGraph hg;
          //cerr << "Start word:" << corpus.intToWords[sentence[0]]<<corpus.intToWords[sentence[1]] << endl;
          std::vector<std::vector<string>> cand;
          if (Opt.beam_size == 0)
              log_prob_parser(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
                              corpus.intToWords, &right, cand);
          else
              log_prob_parser_beam(&hg, sentence, tsentence, sentencePos, actions, corpus.actions,
                                    corpus.intToWords, &right, Opt.beam_size, dg);
          double lp = as_scalar(hg.incremental_forward((VariableIndex)(hg.nodes.size() - 1)));
          if (lp < 0) {
             cerr << "Log prob < 0 on sentence " << order[si] << ": lp=" << lp << endl;
             assert(lp >= 0.0);
          }
           hg.backward((VariableIndex)(hg.nodes.size() - 1));
           sgd.update(1.0);
           llh += lp;
           ++si;
           trs += actions.size();
      }
      sgd.status();
      //time_t time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      time_t time_now = time(NULL);
      std::string t_n(asctime(localtime(&time_now))); 
      cerr << "update #" << iter << " (epoch " << (tot_seen / corpus.nsentences) 
      << " |time=" << t_n.substr(0, t_n.size() - 1) << ")\tllh: "<< llh<<" ppl: " << exp(llh / trs) 
      << " err: " << (trs - right) / trs << endl;
      llh = trs = right = 0;

      static int logc = 0;
      ++logc;
      if (logc % 25 == 1) { // report on dev set
        unsigned dev_size = corpus.nsentencesDev;
        // dev_size = 100;
        double llh = 0;
        double trs = 0;
        double right = 0;
        //double correct_heads = 0;
        //double total_heads = 0;
        auto t_start = std::chrono::high_resolution_clock::now();
        std::vector<std::vector<std::vector<string>>> refs, hyps;
        for (unsigned sii = 0; sii < dev_size; ++sii) {
          const std::vector<unsigned>& sentence=corpus.sentencesDev[sii];
          const std::vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii]; 
          const std::vector<unsigned>& actions=corpus.correct_act_sentDev[sii];
          std::vector<unsigned> tsentence=sentence;
          for (auto& w : tsentence)
            if (training_vocab.count(w) == 0) w = kUNK;

          ComputationGraph hg;
          std::vector<std::vector<string>> cand;
          std::vector<unsigned> pred;
          if (Opt.beam_size == 0)
            pred = log_prob_parser(&hg, sentence, tsentence, sentencePos, std::vector<unsigned>(),
                                    corpus.actions, corpus.intToWords, &right, cand);
          else
            pred = log_prob_parser_beam(&hg, sentence, tsentence, sentencePos, std::vector<unsigned>(), 
                                    corpus.actions, corpus.intToWords, &right, Opt.beam_size, dg);
          double lp = 0;
          llh -= lp;
          trs += actions.size();
          //cerr << "start word:" << sii << corpus.intToWords[sentence[0]] << corpus.intToWords[sentence[1]] << endl;
          std::vector<std::vector<string>> ref = compute_heads(sentence, actions);
          std::vector<std::vector<string>> hyp = compute_heads(sentence, pred);
          //output_conll(sentence, corpus.intToWords, ref, hyp);
          //correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
          //total_heads += sentence.size() - 1;
          refs.push_back(ref);
          hyps.push_back(hyp);
        }
        map<string, double> results = evaluate(refs, hyps);
        auto t_end = std::chrono::high_resolution_clock::now();
        cerr << "  **dev (iter=" << iter << " epoch=" << (tot_seen / corpus.nsentences) << ")\tllh=" << llh 
                << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " LF: " << results["LF"] << " UF:" << results["UF"] 
                << " NLF: " << results["NLF"] << " NUF:" << results["NUF"]
                << " LP:" << results["LP"] << " LR:" << results["LR"] << " UP:" << results["UP"] << " UR:" <<results["UR"]
                << "\t[" << dev_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;

        if (results["LF"] > best_LF) {
          cerr << "---saving model to " << fname << "---" << endl;
          best_LF = results["LF"];
          ofstream out(fname);
          boost::archive::text_oarchive oa(out);
          oa << model;
          // Create a soft link to the most recent model in order to make it
          // easier to refer to it in a shell script.
          if (!softlinkCreated) {
            string softlink = " latest_model";
            if (system((string("rm -f ") + softlink).c_str()) == 0 && 
                system((string("ln -s ") + fname + softlink).c_str()) == 0) {
              cerr << "Created " << softlink << " as a soft link to " << fname 
                   << " for convenience." << endl;
            }
            softlinkCreated = true;
          }
        }
      }
    }//while
}

void LSTMParser::predict_dev() {
    double llh = 0;
    double trs = 0;
    double right = 0;
    //double correct_heads = 0;
    //double total_heads = 0;
    std::vector<std::vector<std::vector<string>>> refs, hyps;
    auto t_start = std::chrono::high_resolution_clock::now();
    unsigned corpus_size = corpus.nsentencesDev;

    int miss_head = 0;

    for (unsigned sii = 0; sii < corpus_size; ++sii) {
      const std::vector<unsigned>& sentence = corpus.sentencesDev[sii];
      const std::vector<unsigned>& sentencePos = corpus.sentencesPosDev[sii];
      const std::vector<string>& sentenceUnkStr = corpus.sentencesStrDev[sii]; 
      const std::vector<unsigned>& actions = corpus.correct_act_sentDev[sii];
      std::vector<unsigned> tsentence=sentence;
      for (auto& w : tsentence)
        if (training_vocab.count(w) == 0) w = kUNK;
      double lp = 0;
      std::vector<unsigned> pred;
      std::vector<std::vector<string>> cand;
      std::vector<Expression> word_rep; // word representations
      Expression act_rep; // final action representation
      //cerr<<"compute action" << endl;

      {
      ComputationGraph cg;
      if (Opt.beam_size == 0)
          pred = log_prob_parser(&cg, sentence, tsentence, sentencePos, std::vector<unsigned>(), 
                              corpus.actions,corpus.intToWords, &right, cand, &word_rep, &act_rep);
      else
          pred = log_prob_parser_beam(&cg, sentence, tsentence, sentencePos, std::vector<unsigned>(), 
                                    corpus.actions, corpus.intToWords, &right, Opt.beam_size, dg);
      }
      /*cerr << cand.size() << endl;
      for (unsigned i = 0; i < cand.size(); ++i){
        for (unsigned j = 0; j < cand.size(); ++j){
            if (cand[i][j] != REL_NULL)
                cerr << "from " << i << " to " << j << " rel: " << cand[i][j] << endl;
        }
      }*/
      llh -= lp;
      trs += actions.size();
      //map<int, string> rel_ref, rel_hyp;
      //cerr << "compute heads "<<endl;
      std::vector<std::vector<string>> ref = compute_heads(sentence, actions);
      std::vector<std::vector<string>> hyp = compute_heads(sentence, pred);
      if (process_headless(hyp, cand, word_rep, act_rep, sentence, sentencePos) > 0) {
            miss_head++;
            cerr << corpus.intToWords[sentence[0]] << corpus.intToWords[sentence[1]]<< endl;
      }
      refs.push_back(ref);
      hyps.push_back(hyp);

      /*for (unsigned i = 0; i < hyp.size(); ++i){
        for (unsigned j = 0; j < hyp.size(); ++j){
            if (hyp[i][j] != REL_NULL)
                cerr << "from " << i << " to " << j << " rel: " << hyp[i][j] << endl;
        }
      }*/
      if (sii%100 == 0)
         cerr << "sentence: " << sii << endl;
      //cerr<<"write to file" <<endl;
      output_conll(sentence, sentencePos, sentenceUnkStr, hyp);
      //correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
      //total_heads += sentence.size() - 1;
    }
    
    /*for (unsigned sii = 0; sii < corpus_size; ++sii) {
      const std::vector<unsigned>& sentence = corpus.sentencesDev[sii];
      const std::vector<unsigned>& sentencePos = corpus.sentencesPosDev[sii];
      const std::vector<string>& sentenceUnkStr = corpus.sentencesStrDev[sii]; 
      std::vector<std::vector<string>> hyp = hyps[sii];
      output_conll(sentence, sentencePos, sentenceUnkStr, hyp);
    }*/ 

    //cerr << "miss head number: " << miss_head << endl;
    map<string, double> results = evaluate(refs, hyps);
    auto t_end = std::chrono::high_resolution_clock::now();
      cerr << "TEST llh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs 
      << " LF: " << results["LF"] << " UF:" << results["UF"]  << " LP:" << results["LP"] << " LR:" << results["LR"] 
      << " UP:" << results["UP"] << " UR:" <<results["UR"]  << "\t[" << corpus_size << " sents in " 
      << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
}

void LSTMParser::predict(std::vector<std::vector<string>> &hyp, const std::vector<std::string> & words,
                          const std::vector<std::string> & postags) {
    std::vector<unsigned> sentence;
    std::vector<unsigned> sentencePos;
    std::vector<std::string> sentenceUnkStr;
    std::string word;
    std::string pos;

    for (int i = 0; i < words.size(); i++){
        word = words[i];
        pos = postags[i];
        // new POS tag
        if (corpus.posToInt[pos] == 0) {
            corpus.posToInt[pos] = corpus.maxPos;
            corpus.intToPos[corpus.maxPos] = pos;
            corpus.npos = corpus.maxPos;
            corpus.maxPos++;
        }
        // add an empty string for any token except OOVs (it is easy to 
        // recover the surface form of non-OOV using intToWords(id)).
        sentenceUnkStr.push_back("");
        // OOV word
        if (corpus.wordsToInt[word] == 0) {
            if (corpus.USE_SPELLING) {
              corpus.max = corpus.nwords + 1;
              //std::cerr<< "max:" << max << "\n";
              corpus.wordsToInt[word] = corpus.max;
              corpus.intToWords[corpus.max] = word;
              corpus.nwords = corpus.max;
            } else {
              // save the surface form of this OOV before overwriting it.
              sentenceUnkStr[sentenceUnkStr.size()-1] = word;
              word = corpus.UNK;
            }
        }
        sentence.push_back(corpus.wordsToInt[word]);
        sentencePos.push_back(corpus.posToInt[pos]);
    }

    std::vector<unsigned> tsentence=sentence;
      for (auto& w : tsentence)
        if (training_vocab.count(w) == 0) w = kUNK;
      std::vector<unsigned> pred;
      std::vector<std::vector<string>> cand;
      std::vector<Expression> word_rep; // word representations
      Expression act_rep; // final action representation
      double right = 0;
      {
      ComputationGraph cg;
      if (Opt.beam_size == 0)
          pred = log_prob_parser(&cg, sentence, tsentence, sentencePos, std::vector<unsigned>(), 
                              corpus.actions,corpus.intToWords, &right, cand, &word_rep, &act_rep);
      else
          pred = log_prob_parser_beam(&cg, sentence, tsentence, sentencePos, std::vector<unsigned>(), 
                                    corpus.actions, corpus.intToWords, &right, Opt.beam_size, dg);

      }
      hyp = compute_heads(sentence, pred);
      //cerr << "hyp length: " << hyp.size() << " " << hyp[0].size() << endl;
      if (process_headless(hyp, cand, word_rep, act_rep, sentence, sentencePos) > 0) {
            cerr << corpus.intToWords[sentence[0]] << corpus.intToWords[sentence[1]]<< endl;
      }
    //output_conll(sentence, sentencePos, sentenceUnkStr, hyp);
}

void LSTMParser::output_conll(const vector<unsigned>& sentence, const vector<unsigned>& pos,
                  const vector<string>& sentenceUnkStrings, 
                  const vector<vector<string>>& hyp) {
    const map<unsigned, string>& intToWords = corpus.intToWords;
    const map<unsigned, string>& intToPos = corpus.intToPos;
    for (unsigned i = 0; i < (sentence.size()-1); ++i) {
        auto index = i + 1;
        assert(i < sentenceUnkStrings.size() && 
            ((sentence[i] == corpus.get_or_add_word(cpyp::Corpus::UNK) &&
                sentenceUnkStrings[i].size() > 0) ||
                (sentence[i] != corpus.get_or_add_word(cpyp::Corpus::UNK) &&
                sentenceUnkStrings[i].size() == 0 &&
                intToWords.find(sentence[i]) != intToWords.end())));
        string wit = (sentenceUnkStrings[i].size() > 0)? 
        sentenceUnkStrings[i] : intToWords.find(sentence[i])->second;
        auto pit = intToPos.find(pos[i]);
        for (unsigned j = 0; j < sentence.size() ; ++j){
            if (hyp[j][i] != ltp::lstmsdparser::REL_NULL){
                auto hyp_head = j + 1;
                if (hyp_head == sentence.size()) hyp_head = 0;
                auto hyp_rel = hyp[j][i];
                cout << index << '\t'       // 1. ID 
                    << wit << '\t'         // 2. FORM
                    << "_" << '\t'         // 3. LEMMA 
                    << "_" << '\t'         // 4. CPOSTAG 
                    << pit->second << '\t' // 5. POSTAG
                    << "_" << '\t'         // 6. FEATS
                    << hyp_head << '\t'    // 7. HEAD
                    << hyp_rel << '\t'     // 8. DEPREL
                    << "_" << '\t'         // 9. PHEAD
                    << "_" << endl;        // 10. PDEPREL
            }
        }
  }
  cout << endl;
}


map<string, double> LSTMParser::evaluate(const vector<vector<vector<string>>>& refs, const vector<vector<vector<string>>>& hyps) {

    std::map<int, std::vector<unsigned>>& sentencesPos = corpus.sentencesPosDev;
    const unsigned punc = corpus.posToInt["PU"];
    assert(refs.size() == hyps.size());
    int correct_arcs = 0; // unlabeled
    int correct_arcs_wo_punc = 0;
    int correct_rels = 0; // labeled
    int correct_rels_wo_punc = 0;

    //int correct_labeled_trees = 0;
    int correct_labeled_graphs_wo_punc = 0;
    //int correct_unlabeled_trees = 0;
    int correct_unlabeled_graphs_wo_punc = 0;
    //int correct_root = 0;

    int sum_gold_arcs = 0;
    int sum_gold_arcs_wo_punc = 0;
    int sum_pred_arcs = 0;
    int sum_pred_arcs_wo_punc =0;

    bool correct_labeled_flag_wo_punc = true;
    bool correct_unlabeled_flag_wo_punc = true;

    int correct_non_local_arcs = 0;
    int correct_non_local_rels = 0;

    int sum_non_local_gold_arcs = 0;
    int sum_non_local_pred_arcs = 0;
    for (int i = 0; i < (int)refs.size(); ++i){
        vector<unsigned> sentPos = sentencesPos[i];
        unsigned sent_len = refs[i].size();
        assert(sentPos.size() == sent_len);
        vector<int> gold_head(sent_len, 0);
        vector<int> pred_head(sent_len, 0);
        correct_labeled_flag_wo_punc = true;
        correct_unlabeled_flag_wo_punc = true;
        for (unsigned j = 0; j < sent_len; ++j){
            for (unsigned k = 0; k < sent_len; ++k){
                if (refs[i][j][k] != REL_NULL){
                    sum_gold_arcs ++;
                    //cerr << " id : " << k + 1 << " POS: " << sentPos[k] << endl;
                    if (sentPos[k] != punc){
                        sum_gold_arcs_wo_punc ++;
                        gold_head[k]++;
                    }
                    if (hyps[i][j][k] != REL_NULL){
                        correct_arcs ++;
                        if (sentPos[k] != punc)
                            correct_arcs_wo_punc ++;
                        if (hyps[i][j][k] == refs[i][j][k]){
                            correct_rels ++;
                            if (sentPos[k] != punc)
                                correct_rels_wo_punc ++;
                        }
                        else if (sentPos[k] != punc){
                            correct_labeled_flag_wo_punc = false;
                        }
                    }
                    else if (sentPos[k] != punc){
                            correct_labeled_flag_wo_punc = false;
                            correct_unlabeled_flag_wo_punc = false;
                    }
                }
                if (hyps[i][j][k] != REL_NULL){
                    sum_pred_arcs ++;
                    if (sentPos[k] != punc){
                        sum_pred_arcs_wo_punc ++;
                        pred_head[k] ++;
                    }
                }
            }//k
        }//j
        if (correct_unlabeled_flag_wo_punc){
            correct_unlabeled_graphs_wo_punc ++;
            if (correct_labeled_flag_wo_punc)
                correct_labeled_graphs_wo_punc ++;
        }
        for (unsigned c = 0; c < sent_len; ++c){
            if (gold_head[c] == 1 && pred_head[c] == 1)
                continue;
            sum_non_local_gold_arcs += gold_head[c];
            sum_non_local_pred_arcs += pred_head[c];
            for (unsigned h = 0; h < sent_len; ++h){
                if (refs[i][h][c] != REL_NULL && sentPos[c] != punc 
                    && hyps[i][h][c] != REL_NULL){
                    correct_non_local_arcs ++;
                    if (refs[i][h][c] == hyps[i][h][c]){
                        correct_non_local_rels ++;
                    }
                }          
            }//h
        }//c
    }//i
    //int sum_graphs = (int)refs.size();
    //cerr << "cor: arcs: " << correct_arcs_wo_punc << " rels: " << correct_rels_wo_punc 
            //<< "\nsum: gold arcs: " << sum_gold_arcs_wo_punc << " pred arcs: " << sum_pred_arcs_wo_punc << endl;
    map<string, double> result;
    if (sum_non_local_gold_arcs == 0)
      sum_non_local_gold_arcs = 1;
    if (sum_non_local_pred_arcs == 0)
      sum_non_local_pred_arcs = 1;
    result["UR"] = correct_arcs_wo_punc * 100.0 / sum_gold_arcs_wo_punc;
    result["UP"] = correct_arcs_wo_punc * 100.0 / sum_pred_arcs_wo_punc;
    result["LR"] = correct_rels_wo_punc * 100.0 / sum_gold_arcs_wo_punc;
    result["LP"] = correct_rels_wo_punc * 100.0 / sum_pred_arcs_wo_punc;

    result["NUR"] = correct_non_local_arcs * 100.0 / sum_non_local_gold_arcs;
    result["NUP"] = correct_non_local_arcs * 100.0 / sum_non_local_pred_arcs;
    result["NLR"] = correct_non_local_rels * 100.0 / sum_non_local_gold_arcs;
    result["NLP"] = correct_non_local_rels * 100.0 / sum_non_local_pred_arcs;

    if (sum_pred_arcs_wo_punc == 0){
        result["LP"] = 0;
        result["UP"] = 0;
    }

    result["UF"] = 2 * result["UR"] * result["UP"] / (result["UR"] + result["UP"]);   
    result["LF"] = 2 * result["LR"] * result["LP"] / (result["LR"] + result["LP"]);

    result["NUF"] = 2 * result["NUR"] * result["NUP"] / (result["NUR"] + result["NUP"]);   
    result["NLF"] = 2 * result["NLR"] * result["NLP"] / (result["NLR"] + result["NLP"]);

    if (result["LR"] == 0 && result["LP"] == 0)
        result["LF"] = 0;
    if (result["UR"] == 0 && result["UP"] == 0)
        result["UF"] = 0;
    if (result["NLR"] == 0 && result["NLP"] == 0)
        result["NLF"] = 0;
    if (result["NUR"] == 0 && result["NUP"] == 0)
        result["NUF"] = 0;
    return result;
}



} //  namespace lstmsdparser
} //  namespace ltp
