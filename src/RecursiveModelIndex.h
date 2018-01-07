/**
 * @file RecursiveModelIndex.h
 *
 * @breif An implementation of the Recursive Model Index concept
 *
 * @date 1/07/2018
 * @author Ben Caine
 */

#ifndef LEARNED_INDICES_RECURSIVEMODELINDEX_H
#define LEARNED_INDICES_RECURSIVEMODELINDEX_H

#include "utils/DataUtils.h"
#include "../external/nn_cpp/nn/Net.h"
#include "../external/cpp-btree/btree_map.h"

/**
 * @brief A container for the hyperparameters of our first level network
 */
struct NetworkParameters {
    int batchSize;      ///< The batch size of our network
    int maxNumEpochs;   ///< The max number of epochs to train the network for
    float learningRate; ///< The learning rate of our Adam solver
    int numNeurons;     ///< The number of neurons
};

/**
 * @brief An implementation of the recursive model index
 * @tparam KeyType: The key type of our index
 * @tparam ValueType: The value we are storing
 * @tparam secondStageSize: The size of our second stage of our index
 */
template <typename KeyType, typename ValueType, int secondStageSize>
class RecursiveModelIndex {
public:

    /**
     * @brief Create a RMI
     * @param networkParameters [in]: The first layer network parameters
     * @param maxOverflowSize [in]: The max size our overflow BTree can get to before we force a retrain
     */
    explicit RecursiveModelIndex(const NetworkParameters &networkParameters, int maxOverflowSize = 10000);

    //TODO: Is it more common to pass a pair?
    /**
     * @brief Insert into our index new data
     * @param key [in]: The key to insert
     * @param value [in]: The value to insert
     */
    void insert(KeyType key, ValueType value);

    //TODO: What to do if not found
    /**
     * @brief Find a specific item from the tree
     * @param key [in]: A key to search for
     * @return A pair of (key, value) if found.
     */
    std::pair<KeyType, ValueType> find(KeyType key);

    /**
     * @brief Train our index structure
     */
    void train();

private:

    /**
     * @brief Train the first stage of the network
     */
    void trainFirstStage();

    /**
     * @brief train the second stage linear models of the network
     */
    void trainSecondStage();

    ///------------ Data members ----------------

    std::vector<std::pair<KeyType, ValueType>> m_data;                 ///< The data our learned index tries to find

    NetworkParameters m_networkParameters;                             ///< First stage network parameters
    std::unique_ptr<nn::Net<float>> m_firstStageNetwork;               ///< The first stage neural network
    std::array<nn::Net<float>, secondStageSize> m_secondStageNetworks; ///< The second stage networks

    int m_currentOverflowSize;                                         ///< Number of inserts stored in overflow array
    int m_maxOverflowSize;                                             ///< Max size we let overflow array get before retraining
    std::vector<std::pair<KeyType, ValueType>> m_overflowArray;        ///< The overflow array
};

template <typename KeyType, typename ValueType, int secondStageSize>
RecursiveModelIndex<KeyType, ValueType, secondStageSize>::RecursiveModelIndex(const NetworkParameters &networkParameters, int maxOverflowSize):
    m_networkParameters(networkParameters), m_maxOverflowSize(maxOverflowSize)
{

    // Create our first network
    m_firstStageNetwork.reset(new nn::Net<float>());
    m_firstStageNetwork->add(new nn::Dense<float, 2>(networkParameters.batchSize, 1, networkParameters.numNeurons, true, nn::InitializationScheme::GlorotNormal));
    m_firstStageNetwork->add(new nn::Relu<float, 2>());
    m_firstStageNetwork->add(new nn::Dense<float, 2>(networkParameters.batchSize, networkParameters.numNeurons, 1, true, nn::InitializationScheme::GlorotNormal));

    // Create all our linear models
    for (size_t ii = 0; ii < secondStageSize; ++ii) {
        m_secondStageNetworks[ii] = nn::Net<float>();
        m_secondStageNetworks[ii].add(new nn::Dense<float, 2>(networkParameters.batchSize, 1, 1, true, nn::InitializationScheme::GlorotNormal));
    }
}

template <typename KeyType, typename ValueType, int secondStageSize>
void RecursiveModelIndex<KeyType, ValueType, secondStageSize>::insert(KeyType key, ValueType value) {
    m_overflowArray.push_back({key, value});
    m_currentOverflowSize ++;

    // TODO: This should really be a background task
    if (m_currentOverflowSize > m_maxOverflowSize) {
        train();
    }
};

template <typename KeyType, typename ValueType, int secondStageSize>
std::pair<KeyType, ValueType> RecursiveModelIndex<KeyType, ValueType, secondStageSize>::find(KeyType key) {
    // TODO: Order of searching?
    auto result = std::find_if(m_overflowArray.begin(), m_overflowArray.end(), [&](const std::pair<KeyType, ValueType> &pair) {
        return pair.first == key;
    });

    if (result != m_overflowArray.end()) {
        return *result;
    }

    return {};
};

template <typename KeyType, typename ValueType, int secondStageSize>
void RecursiveModelIndex<KeyType, ValueType, secondStageSize>::train() {
    std::cout << "Retraining..." << std::endl;
    m_data.insert(m_data.end(), m_overflowArray.begin(), m_overflowArray.end());

    // Sort data
    std::sort(m_data.begin(), m_data.end(), [](std::pair<KeyType, ValueType> p1, std::pair<KeyType, ValueType> p2) {
        return p1.first < p2.first;
    });

    trainFirstStage();
    trainSecondStage();

    // Clear out overflow tree
    m_overflowArray.clear();
    m_currentOverflowSize = 0;
}

template <typename KeyType, typename ValueType, int secondStageSize>
void RecursiveModelIndex<KeyType, ValueType, secondStageSize>::trainFirstStage() {
    // TODO: Do we want to clear out the old network or use it's previous weights?
    std::cout << "Training first stage" << std::endl;

    // Huber loss is used for increased stability
    nn::HuberLoss<float, 2> lossFunction;

    // Adam because vanilla SGD doesn't converge at all
    m_firstStageNetwork->registerOptimizer(new nn::Adam<float>(m_networkParameters.learningRate));

    Eigen::Tensor<float, 2> input(m_networkParameters.batchSize, 1);
    Eigen::Tensor<float, 2> labels(m_networkParameters.batchSize, 1);

    for (int currentEpoch = 0; currentEpoch < m_networkParameters.maxNumEpochs; ++currentEpoch) {
        auto newBatch = getRandomBatch<KeyType>(m_networkParameters.batchSize, m_data.size());
        int ii = 0;
        for (auto idx : newBatch) {
            // Input is the key
            input(ii, 0) = static_cast<float>(m_data[idx].first);
            // Label is the position in our sorted array
            labels(ii, 0) = static_cast<float>(idx);
            ii++;
        }

        auto result = m_firstStageNetwork->forward<2, 2>(input);
        result = result * result.constant(m_data.size());

        auto loss = lossFunction.loss(result, labels);
        // TODO: Add logging, make this Debug
        std::cout << "Epoch: " << currentEpoch << " Loss: " << loss << std::endl;

        auto lossBack = lossFunction.backward(result, labels);
        // Divide loss back by dataset size to stabilize training and remove relationship between
        // learning rate and dataset size
        lossBack = lossBack / lossBack.constant(m_data.size());

        m_firstStageNetwork->backward<2>(lossBack);
        m_firstStageNetwork->step();
    }
}

template <typename KeyType, typename ValueType, int secondStageSize>
void RecursiveModelIndex<KeyType, ValueType, secondStageSize>::trainSecondStage() {
    std::cout << "Training second stage" << std::endl;

    std::array<std::vector<std::pair<KeyType, size_t>>, secondStageSize> perStageDataset;
    Eigen::Tensor<float, 2> predictInput(1, 1);
    for (int ii = 0; ii < m_data.size(); ++ii) {
        std::cout << ii << " - " << m_data[ii].first << std::endl;
        predictInput(0, 0) = static_cast<float>(m_data[ii].first);
        auto result = m_firstStageNetwork->forward<2, 2>(predictInput);
        result = result * result.constant(m_data.size());

        std::cout << "Input: " << m_data[ii].first << " predicted: " << result << " Actual: " << ii <<  std::endl;
    }
}

#endif //LEARNED_INDICES_RECURSIVEMODELINDEX_H